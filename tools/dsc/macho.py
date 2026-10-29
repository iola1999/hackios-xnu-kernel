#!/usr/bin/env python3
"""Mach-O parsing for images embedded in a split dyld shared cache, plus extraction.

Extraction copies the image's file-backed segments into a new file and relocates the
__LINKEDIT blobs it references to new file offsets, patching every load command that
holds a file offset. Layout rules that matter:

  - the mach header sits at the start of the __TEXT file data, so __TEXT goes to
    file offset 0 and the patched load commands overwrite the copied header;
  - every other segment is placed sequentially with fileoff congruent to vmaddr
    modulo the 16 KB page size, and every section offset is recomputed;
  - __LINKEDIT data in the cache is addressed by absolute file offsets inside the
    subcache file that hosts the shared linkedit region, so each referenced blob is
    copied on its own, not as one span;
  - the symbol table keeps its entries but gets a rebuilt string table; the cached
    string table is tens of megabytes of a pool shared by every image.

The output is meant for static tools (otool, llvm-objdump, idalib), not for loading.
"""

import os
import struct

MAGIC_64 = 0xfeedfacf
PAGE = 0x4000

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xb
LC_TWOLEVEL_HINTS = 0x16
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_CODE_SIGNATURE = 0x1d
LC_SEGMENT_SPLIT_INFO = 0x1e
LC_FUNCTION_STARTS = 0x26
LC_DATA_IN_CODE = 0x29
LC_DYLIB_CODE_SIGN_DRS = 0x2b
LC_ATOM_INFO = 0x36
LC_FUNCTION_VARIANTS = 0x37
LC_FUNCTION_VARIANT_FIXUPS = 0x38
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_CHAINED_FIXUPS = 0x80000034

LINKEDIT_DATA_CMDS = {
    LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO, LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
    LC_DYLIB_CODE_SIGN_DRS, LC_ATOM_INFO, LC_FUNCTION_VARIANTS,
    LC_FUNCTION_VARIANT_FIXUPS, LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS,
}

# LC_DYSYMTAB file-offset fields: (byte offset in command, count field, entry size)
DYSYMTAB_FIELDS = (
    (8 + 4 * 6, 8 + 4 * 7, 8),    # tocoff / ntoc, dylib_table_of_contents
    (8 + 4 * 8, 8 + 4 * 9, 56),   # modtaboff / nmodtab, dylib_module_64
    (8 + 4 * 10, 8 + 4 * 11, 4),  # extrefsymoff / nextrefsyms, dylib_reference
    (8 + 4 * 12, 8 + 4 * 13, 4),  # indirectsymoff / nindirectsyms
    (8 + 4 * 14, 8 + 4 * 15, 8),  # extreloff / nextrel, relocation_info
    (8 + 4 * 16, 8 + 4 * 17, 8),  # locreloff / nlocrel
)

ZEROFILL_TYPES = (0x1, 0xc, 0x12)  # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL


class MachOError(Exception):
    pass


class Section:
    __slots__ = ('name', 'segname', 'addr', 'size', 'offset', 'align', 'flags')

    def __init__(self, name, segname, addr, size, offset, align, flags):
        self.name = name
        self.segname = segname
        self.addr = addr
        self.size = size
        self.offset = offset
        self.align = align
        self.flags = flags

    @property
    def is_zerofill(self):
        return (self.flags & 0xff) in ZEROFILL_TYPES


class Segment:
    __slots__ = ('name', 'vmaddr', 'vmsize', 'fileoff', 'filesize', 'maxprot', 'initprot',
                 'sections', 'cmd_offset')

    def __init__(self, name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot,
                 sections, cmd_offset):
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize
        self.maxprot = maxprot
        self.initprot = initprot
        self.sections = sections
        self.cmd_offset = cmd_offset

    def __repr__(self):
        return f'Segment({self.name}, {self.vmaddr:#x}+{self.vmsize:#x})'


class MachO:
    def __init__(self, cache, base):
        self.cache = cache
        self.base = base
        head = cache.read(base, 32)
        (magic, self.cputype, self.cpusubtype, self.filetype,
         self.ncmds, self.sizeofcmds, self.flags, _reserved) = struct.unpack('<IiiIIIII', head)
        if magic != MAGIC_64:
            raise MachOError(f'no 64-bit mach header at {base:#x} (magic {magic:#x})')
        self.body = bytearray(cache.read(base + 32, self.sizeofcmds))
        self.commands = []
        self.segments = []
        p = 0
        for _ in range(self.ncmds):
            if p + 8 > len(self.body):
                raise MachOError(f'load command table overruns at {base:#x}')
            cmd, size = struct.unpack_from('<II', self.body, p)
            if size < 8 or p + size > len(self.body):
                raise MachOError(f'bad load command {cmd:#x} size {size} at {base:#x}')
            self.commands.append((cmd, p, size))
            if cmd == LC_SEGMENT_64:
                self.segments.append(parse_segment(self.body, p, size))
            p += size

    def segment(self, name):
        for seg in self.segments:
            if seg.name == name:
                return seg
        return None

    def find_command(self, cmd):
        for c, p, size in self.commands:
            if c == cmd:
                return p, size
        return None

    def symtab(self):
        found = self.find_command(LC_SYMTAB)
        if not found:
            return None
        p, _ = found
        return struct.unpack_from('<IIII', self.body, p + 8)


def segments(cache, base):
    return MachO(cache, base).segments


def parse_segment(body, p, size):
    name = _cstr(body[p + 8:p + 24])
    vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', body, p + 24)
    maxprot, initprot, nsects, _flags = struct.unpack_from('<IIII', body, p + 56)
    sections = []
    at = p + 72
    for _ in range(nsects):
        if at + 80 > p + size:
            raise MachOError(f'section table overruns in segment {name}')
        sectname = _cstr(body[at:at + 16])
        segname = _cstr(body[at + 16:at + 32])
        addr, secsize = struct.unpack_from('<QQ', body, at + 32)
        offset, align = struct.unpack_from('<II', body, at + 48)
        flags = struct.unpack_from('<I', body, at + 64)[0]
        sections.append(Section(sectname, segname, addr, secsize, offset, align, flags))
        at += 80
    return Segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, sections, p)


def _cstr(raw):
    return raw.split(b'\0', 1)[0].decode('utf-8', 'replace')


def _align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def _align_congruent(value, vmaddr, page=PAGE):
    """File offset aligned to page and congruent to vmaddr modulo the page size."""
    return _align(value - (vmaddr % page), page) + (vmaddr % page)


class Extractor:
    def __init__(self, cache, image, compact_symbols=True, log=print):
        self.cache = cache
        self.image = image
        self.compact_symbols = compact_symbols
        self.log = log
        self.macho = MachO(cache, image.address)
        self.segments = self.macho.segments
        self.linkedit = self.macho.segment('__LINKEDIT')
        self.stats = {'segments': 0, 'linkedit_blobs': 0, 'linkedit_bytes': 0,
                      'symbols': 0, 'strtab_bytes': 0, 'output_bytes': 0}

    # -- source addresses ---------------------------------------------------
    def _linkedit_addr(self, file_off):
        if self.linkedit is None:
            raise MachOError('image has no __LINKEDIT segment')
        return self.linkedit.vmaddr + (file_off - self.linkedit.fileoff)

    def _read_linkedit(self, file_off, size):
        addr = self._linkedit_addr(file_off)
        host, _off, _prot = self.cache.where(addr)
        last_host, _off2, _prot2 = self.cache.where(addr + max(size, 1) - 1)
        if host != last_host:
            raise MachOError(f'linkedit range {file_off:#x}+{size:#x} crosses subcache files')
        return self.cache.read(addr, size)

    # -- symbol table -------------------------------------------------------
    def _rebuild_symtab(self, symoff, nsyms, stroff, strsize):
        raw = self._read_linkedit(symoff, nsyms * 16)
        pool = bytearray(b'\0')
        string_at = {0: 0}
        by_strx = {}
        entries = bytearray(raw)
        for i in range(nsyms):
            strx = struct.unpack_from('<I', entries, 16 * i)[0]
            if not strx or strx >= strsize:
                new = 0
            else:
                if strx not in by_strx:
                    by_strx[strx] = self._read_cstring(stroff + strx, strsize - strx)
                text = by_strx[strx]
                new = string_at.get(text)
                if new is None:
                    new = len(pool)
                    string_at[text] = new
                    pool += text + b'\0'
            struct.pack_into('<I', entries, 16 * i, new)
        self.stats['symbols'] = nsyms
        self.stats['strtab_bytes'] = len(pool)
        return bytes(entries), bytes(pool)

    def _read_cstring(self, file_off, limit):
        limit = min(limit, 1 << 16)
        data = self._read_linkedit(file_off, limit)
        end = data.find(b'\0')
        return data if end < 0 else data[:end]

    # -- layout -------------------------------------------------------------
    def _place_segments(self):
        placements = [None] * len(self.segments)
        text_index = next((i for i, s in enumerate(self.segments) if s.vmaddr == self.image.address),
                          None)
        if text_index is None:
            raise MachOError(f'no segment starts at image base {self.image.address:#x}')
        cursor = 0
        placements[text_index] = 0
        cursor = self.segments[text_index].filesize
        for i, seg in enumerate(self.segments):
            if i == text_index:
                continue
            off = _align_congruent(max(cursor, 0x1000), seg.vmaddr)
            placements[i] = off
            if seg.name != '__LINKEDIT':
                cursor = off + seg.filesize
        return placements

    def _patch_segments(self, placements, linkedit_filesize):
        for seg, new_off in zip(self.segments, placements):
            p = seg.cmd_offset
            struct.pack_into('<Q', self.macho.body, p + 40, new_off)
            if seg.name == '__LINKEDIT' and linkedit_filesize is not None:
                struct.pack_into('<Q', self.macho.body, p + 48, linkedit_filesize)
            for j, sec in enumerate(seg.sections):
                if sec.is_zerofill or sec.size == 0:
                    continue
                sec_off = new_off + (sec.addr - seg.vmaddr)
                if sec_off > 0xffffffff:
                    raise MachOError(f'section {sec.name} lands past 4 GB')
                struct.pack_into('<I', self.macho.body, p + 72 + 80 * j + 48, sec_off)

    def _patch_offsets(self, new_offsets):
        """new_offsets maps (command byte offset, value field offset) -> new value."""
        for (p, field), value in new_offsets.items():
            struct.pack_into('<I', self.macho.body, p + field, value)

    # -- run ----------------------------------------------------------------
    def run(self, out_path):
        if self.linkedit is None:
            raise MachOError('image has no __LINKEDIT segment')
        placements = self._place_segments()
        le_index = next(i for i, s in enumerate(self.segments) if s.name == '__LINKEDIT')
        cursor = max(off + seg.filesize for off, seg in zip(placements, self.segments)
                     if seg.name != '__LINKEDIT')
        le_off = _align_congruent(cursor, self.linkedit.vmaddr)
        placements[le_index] = le_off
        cursor = le_off

        blob_offsets = {}
        patch = {}

        def place(original_off, size, align=8):
            nonlocal cursor
            if original_off in blob_offsets:
                return blob_offsets[original_off][0]
            cursor = _align(cursor, align)
            here = cursor
            cursor += size
            blob_offsets[original_off] = (here, size)
            return here

        # Linkedit data commands and dyld info: one blob each. A zero-size entry keeps
# a stale file offset from the cache, which tools still bounds-check; zero it.
        for cmd, p, _size in self.macho.commands:
            if cmd in LINKEDIT_DATA_CMDS:
                off, size = struct.unpack_from('<II', self.macho.body, p + 8)
                if size:
                    patch[(p, 8)] = place(off, size)
                elif off:
                    patch[(p, 8)] = 0
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                fields = struct.unpack_from('<10I', self.macho.body, p + 8)
                for i in range(5):
                    off, size = fields[2 * i], fields[2 * i + 1]
                    if size:
                        patch[(p, 8 + 4 * (2 * i))] = place(off, size)
                    elif off:
                        patch[(p, 8 + 4 * (2 * i))] = 0
            elif cmd == LC_DYSYMTAB:
                for off_field, count_field, entry_size in DYSYMTAB_FIELDS:
                    off = struct.unpack_from('<I', self.macho.body, p + off_field)[0]
                    count = struct.unpack_from('<I', self.macho.body, p + count_field)[0]
                    if off and count:
                        patch[(p, off_field)] = place(off, count * entry_size)
                    elif off:
                        patch[(p, off_field)] = 0
            elif cmd == LC_TWOLEVEL_HINTS:
                off, count = struct.unpack_from('<II', self.macho.body, p + 8)
                if off and count:
                    patch[(p, 8)] = place(off, count * 4)
                elif off:
                    patch[(p, 8)] = 0

        # Symbol table: rebuilt, or copied verbatim.
        symtab_patch = None
        sym = self.macho.symtab()
        if sym:
            p, _ = self.macho.find_command(LC_SYMTAB)
            symoff, nsyms, stroff, strsize = sym
            if nsyms and symoff:
                if self.compact_symbols:
                    new_sym, new_str = self._rebuild_symtab(symoff, nsyms, stroff, strsize)
                    cursor = _align(cursor, 8)
                    new_symoff = cursor
                    cursor += len(new_sym)
                    new_stroff = cursor
                    cursor += len(new_str)
                    symtab_patch = (p, new_symoff, new_stroff, len(new_str))
                else:
                    patch[(p, 8)] = place(symoff, nsyms * 16, align=8)
                    if stroff and strsize:
                        patch[(p, 16)] = place(stroff, strsize, align=1)
                    self.stats['symbols'] = nsyms

        copied = sorted(blob_offsets.items())
        filesize = cursor - le_off
        if filesize > self.linkedit.vmsize:
            raise MachOError(f'relocated __LINKEDIT ({filesize:#x} bytes) exceeds its vmsize '
                             f'({self.linkedit.vmsize:#x})')
        self._patch_segments(placements, filesize)
        self._patch_offsets(patch)
        if symtab_patch:
            p, symoff, stroff, strsize = symtab_patch
            struct.pack_into('<IIII', self.macho.body, p + 8, symoff, sym[1], stroff, strsize)

        os.makedirs(os.path.dirname(os.path.abspath(out_path)) or '.', exist_ok=True)
        with open(out_path, 'wb') as out:
            out.truncate(cursor)
            for seg, place_off in zip(self.segments, placements):
                if seg.name == '__LINKEDIT' or not seg.filesize:
                    continue
                self._copy(seg.vmaddr, seg.filesize, out, place_off)
                self.stats['segments'] += 1
            if symtab_patch:
                p, symoff, stroff, strsize = symtab_patch
                out.seek(symoff)
                out.write(new_sym)
                out.seek(stroff)
                out.write(new_str)
            for original_off, (here, size) in copied:
                self._copy(self._linkedit_addr(original_off), size, out, here)
                self.stats['linkedit_blobs'] += 1
                self.stats['linkedit_bytes'] += size
            # The mach header lives at the start of __TEXT, so the patched load
            # commands go in after that segment has been copied.
            out.seek(0)
            out.write(struct.pack('<IiiIIIII', MAGIC_64, self.macho.cputype,
                                  self.macho.cpusubtype, self.macho.filetype, self.macho.ncmds,
                                  self.macho.sizeofcmds, self.macho.flags, 0))
            out.write(bytes(self.macho.body))
        self.stats['output_bytes'] = cursor
        return self.stats

    def _copy(self, addr, size, out, out_off, chunk=4 << 20):
        out.seek(out_off)
        done = 0
        while done < size:
            n = min(chunk, size - done)
            out.write(self.cache.read(addr + done, n))
            done += n


def extract(cache, image, out_path, compact_symbols=True, log=print):
    return Extractor(cache, image, compact_symbols=compact_symbols, log=log).run(out_path)


def _uleb(buf, p):
    result = 0
    shift = 0
    while True:
        byte = buf[p]
        p += 1
        result |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return result, p
        shift += 7


def _walk_trie(buf, node, prefix, out, depth=0):
    if depth > 64:
        raise MachOError('export trie nesting is too deep')
    p = node
    terminal_size, p = _uleb(buf, p)
    if terminal_size:
        flags, q = _uleb(buf, p)
        if flags & 0x08:  # EXPORT_SYMBOL_FLAGS_REEXPORT
            ordinal, q = _uleb(buf, q)
            name, q = _uleb(buf, q)
            out.append((prefix, 'reexport', ordinal, name))
        elif flags & 0x10:  # EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER
            address, q = _uleb(buf, q)
            resolver, q = _uleb(buf, q)
            out.append((prefix, 'stub+resolver', address, resolver))
        else:
            address, q = _uleb(buf, q)
            out.append((prefix, 'regular', address, 0))
        p += terminal_size
    children = buf[p]
    p += 1
    for _ in range(children):
        end = buf.index(b'\0', p)
        edge = buf[p:end].decode('utf-8', 'replace')
        p = end + 1
        child, p = _uleb(buf, p)
        _walk_trie(buf, child, prefix + edge, out, depth + 1)


def exports(cache, image):
    """(name, kind, value, extra) for every export; value is an address for regular ones."""
    mo = MachO(cache, image.address)
    linkedit = mo.segment('__LINKEDIT')
    if linkedit is None:
        return []
    off = size = 0
    found = mo.find_command(LC_DYLD_EXPORTS_TRIE)
    if found:
        p, _ = found
        off, size = struct.unpack_from('<II', mo.body, p + 8)
    else:
        for cmd in (LC_DYLD_INFO_ONLY, LC_DYLD_INFO):
            found = mo.find_command(cmd)
            if found:
                p, _ = found
                off, size = struct.unpack_from('<II', mo.body, p + 40)
                break
    if not off or not size:
        return []
    blob = cache.read(linkedit.vmaddr + (off - linkedit.fileoff), size)
    raw = []
    _walk_trie(blob, 0, '', raw)
    return [(name, kind, image.address + value if kind == 'regular' else value, extra)
            for name, kind, value, extra in raw]


def function_starts(cache, image):
    """Sorted nlist values from LC_SYMTAB; cached names are redacted but addresses are real."""
    mo = MachO(cache, image.address)
    linkedit = mo.segment('__LINKEDIT')
    sym = mo.symtab()
    if linkedit is None or not sym or not sym[1] or not sym[0]:
        return []
    raw = cache.read(linkedit.vmaddr + (sym[0] - linkedit.fileoff), sym[1] * 16)
    starts = {struct.unpack_from('<Q', raw, 16 * i + 8)[0] for i in range(sym[1])}
    starts.discard(0)
    return sorted(starts)
#!/usr/bin/env python3
"""ARM64 disassembly and cross-reference scanning over cache-backed code.

Capstone resolves adrp immediates to absolute page addresses, so a register page map
built while iterating is enough to turn `adrp x8, page` + `add x9, x8, #imm` into a
concrete data address. Everything here streams instructions; a full QuartzCore
__TEXT scan is about a million instructions, which stays a few seconds.
"""

import re

import cache
import macho

_CAPSTONE = None
_MEMORY = re.compile(r'\[(\w+)(?:,\s*#(-?(?:0x[0-9a-fA-F]+|\d+)))?\]')
ERRORS = (cache.CacheError, macho.MachOError)


def capstone_md():
    global _CAPSTONE
    if _CAPSTONE is None:
        try:
            import capstone
        except ImportError as exc:
            raise SystemExit(f'dsc.py: capstone is required for this command: {exc}')
        _CAPSTONE = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        _CAPSTONE.skipdata = True
    return _CAPSTONE


def text_segments(cache, image):
    return [seg for seg in macho.segments(cache, image.address)
            if seg.initprot & 0x4 and seg.filesize]


def read_window(cache, addr, length):
    """Read code bytes, clamped to the end of the containing segment."""
    image = cache.image_of(addr)
    if image is None:
        raise macho.MachOError(f'no image contains {addr:#x}')
    for seg in macho.segments(cache, image.address):
        if seg.vmaddr <= addr < seg.vmaddr + seg.filesize:
            length = min(length, seg.vmaddr + seg.filesize - addr)
            return cache.read(addr, length)
    raise macho.MachOError(f'address {addr:#x} is not inside file-backed data of {image.path}')


def _memory_operand(op_str):
    """('x8', 0x10) for an operand written as [x8, #0x10]; None otherwise."""
    m = _MEMORY.search(op_str)
    if not m:
        return None
    return m.group(1), int(m.group(2), 0) if m.group(2) else 0


class Resolver:
    """Tracks adrp pages and exact adrp+add results per register."""

    WRITERS = {'ldr', 'ldrb', 'ldrh', 'ldrsw', 'ldur', 'mov', 'movz', 'movk', 'orr', 'and',
               'eor', 'lsl', 'lsr', 'asr', 'sub', 'ubfx', 'sbfx', 'csel', 'cset', 'adr',
               'neg', 'mvn', 'bfi', 'bfxil', 'sxtw', 'uxtw'}
    MEMORY = {'ldr', 'ldrb', 'ldrh', 'ldrsw', 'ldur', 'str', 'strb', 'strh', 'stur'}

    def __init__(self):
        self.pages = {}
        self.values = {}

    def _address(self, reg):
        if reg in self.values:
            return self.values[reg]
        return self.pages.get(reg)

    def target(self, insn):
        """(kind, address) when the instruction forms an absolute address, else None."""
        m = insn.mnemonic
        parts = [p.strip() for p in insn.op_str.split(',')]
        if m == 'adrp':
            if len(parts) == 2 and parts[1].startswith('#'):
                try:
                    self.pages[parts[0]] = int(parts[1][1:], 0)
                except ValueError:
                    pass
            self.values.pop(parts[0], None)
            return None
        if m == 'add':
            if len(parts) >= 3 and parts[2].startswith('#') and 'lsl' not in insn.op_str:
                base = self._address(parts[1])
                if base is not None:
                    try:
                        value = base + int(parts[2][1:], 0)
                    except ValueError:
                        value = None
                    if value is not None:
                        self.values[parts[0]] = value
                        self.pages.pop(parts[0], None)
                        return 'data', value
            self.values.pop(parts[0], None)
            return None
        if m in self.MEMORY:
            dst = parts[0] if parts else None
            mem = _memory_operand(insn.op_str)
            base = self._address(mem[0]) if mem else None
            if dst:
                self.values.pop(dst, None)
                self.pages.pop(dst, None)
            if base is not None:
                return 'data', base + mem[1]
            return None
        if m in self.WRITERS and parts:
            self.values.pop(parts[0], None)
            self.pages.pop(parts[0], None)
        return None


def _printable(cache_, addr, limit=48):
    try:
        raw = cache_.read(addr, limit)
    except ERRORS:
        return None
    end = raw.find(b'\0')
    if end < 0:
        return None
    text = raw[:end]
    if len(text) < 4 or not all(32 <= b < 127 for b in text):
        return None
    return text.decode('ascii')


def describe_target(cache, addr):
    image = cache.image_of(addr)
    where = f'{addr:#x}'
    if image is not None:
        where += f' ({image.name})'
    text = _printable(cache, addr)
    if text:
        where += f' "{text}"'
    return where


def disasm(cache, addr, count=40, annotate=True, stop_on_ret=False):
    md = capstone_md()
    data = read_window(cache, addr, count * 4)
    resolver = Resolver()
    lines = []
    for insn in md.disasm(data, addr):
        text = f'{insn.address:#012x}: {insn.mnemonic:8s} {insn.op_str}'
        note = ''
        if annotate:
            hit = resolver.target(insn)
            if hit:
                note = '   ; -> ' + describe_target(cache, hit[1])
            elif insn.mnemonic in ('bl', 'b') and insn.op_str.startswith('#'):
                try:
                    target = int(insn.op_str[1:], 0)
                    note = '   ; -> ' + describe_target(cache, target)
                except ValueError:
                    pass
        lines.append(text + note)
        if stop_on_ret and insn.mnemonic in ('ret', 'retab') and len(lines) > 2:
            break
    return lines


def function_strings(cache, addr, max_insns=120):
    """String literals referenced from the start of a function window."""
    md = capstone_md()
    try:
        data = read_window(cache, addr, max_insns * 4)
    except ERRORS:
        return set()
    resolver = Resolver()
    out = set()
    for n, insn in enumerate(md.disasm(data, addr)):
        hit = resolver.target(insn)
        if hit:
            text = _printable(cache, hit[1])
            if text:
                out.add(text)
        if n and insn.mnemonic in ('ret', 'retab') and n > 2:
            break
    return out


def scan_refs(cache, image, targets, mode='all', limit=1000):
    """Instruction addresses that reference any target address.

    mode 'call': direct bl/b immediates; 'data': adrp-based addresses; 'all': both.
    Returns (hits, scanned_instruction_count) where hits are (insn_addr, kind, target).
    """
    md = capstone_md()
    wanted = set(targets)
    hits = []
    scanned = 0
    for seg in text_segments(cache, image):
        resolver = Resolver()
        data = cache.read(seg.vmaddr, seg.filesize)
        for insn in md.disasm(data, seg.vmaddr):
            scanned += 1
            if mode in ('all', 'call') and insn.mnemonic == 'bl' and insn.op_str.startswith('#'):
                try:
                    target = int(insn.op_str[1:], 0)
                except ValueError:
                    target = None
                if target in wanted:
                    hits.append((insn.address, 'call', target))
            if mode in ('all', 'data'):
                hit = resolver.target(insn)
                if hit and hit[1] in wanted:
                    hits.append((insn.address, 'data', hit[1]))
            if len(hits) >= limit:
                return hits, scanned
    return hits, scanned


def hexdump(cache, addr, length):
    data = cache.read(addr, length)
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = ' '.join(f'{b:02x}' for b in chunk)
        text = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'{addr + off:#012x}  {hexpart:47s}  {text}')
    return lines


LDR_PLAIN = re.compile(r'^(x\d+),\s*\[(x\d+)\]$')
LDR_OFFSET = re.compile(r'^(x\d+),\s*\[(x\d+),\s*#(0x[0-9a-fA-F]+|\d+)\]$')
ADD_REG = re.compile(r'^(x\d+),\s*(x\d+),\s*(x\d+)$')
ADD_IMM = re.compile(r'^(x\d+),\s*(x\d+),\s*#(0x[0-9a-fA-F]+|\d+)$')
MOV_IMM = re.compile(r'^(x\d+),\s*#(0x[0-9a-fA-F]+|\d+)(?:,\s*lsl #0)?$')

INDIRECT = ('blr', 'blraa', 'blraaz', 'blrab', 'blrabz',
            'br', 'braa', 'braaz', 'brab', 'brabz')


def scan_virtual_calls(cache, image, offsets=None, limit=1000):
    """Virtual dispatch sites with their vtable offset.

    Two forms exist in these caches. arm64 loads the function pointer with
    `ldr xN, [xM, #off]` and calls it with blr. arm64e authenticates the vtable
    pointer (autda), adds the offset with an `add` immediate, then loads and uses
    blraa/blraaz, so the offset never appears in an ldr.

    Returns (hits, scanned) with hits of (instruction address, slot offset).
    """
    md = capstone_md()
    md.detail = True
    names = {}
    wanted = set(offsets) if offsets else None
    hits = []
    scanned = 0

    def written(insn):
        try:
            _read, write = insn.regs_access()
        except Exception:
            return ()
        out = []
        for rid in write:
            name = names.get(rid)
            if name is None:
                name = md.reg_name(rid)
                names[rid] = name
            out.append(name)
        return out

    for seg in text_segments(cache, image):
        roles = {}
        imms = {}
        data = cache.read(seg.vmaddr, seg.filesize)
        for insn in md.disasm(data, seg.vmaddr):
            scanned += 1
            m = insn.mnemonic
            assign = {}
            imm_assign = {}
            hit = None
            ops = insn.op_str
            if m == 'ldr':
                mo = LDR_PLAIN.match(ops)
                moff = LDR_OFFSET.match(ops)
                if moff:
                    dst, base, imm = moff.group(1), moff.group(2), int(moff.group(3), 0)
                    role = roles.get(base)
                    if role and role[0] in ('ptr', 'authvtable', 'slot'):
                        assign[dst] = ('fn', role[1] if role[0] == 'slot' else imm)
                elif mo:
                    dst, base = mo.group(1), mo.group(2)
                    role = roles.get(base)
                    if role and role[0] == 'slot':
                        assign[dst] = ('fn', role[1])
                    else:
                        assign[dst] = ('ptr', 0)
            elif m.startswith('autd'):
                dst = ops.split(',')[0].strip()
                role = roles.get(dst)
                if role and role[0] == 'ptr':
                    assign[dst] = ('authvtable', 0)
            elif m in ('mov', 'movz'):
                mv = MOV_IMM.match(ops)
                if mv:
                    imm_assign[mv.group(1)] = int(mv.group(2), 0)
            elif m == 'add':
                ai = ADD_IMM.match(ops)
                ar = ADD_REG.match(ops)
                if ai:
                    dst, base, imm = ai.group(1), ai.group(2), int(ai.group(3), 0)
                    role = roles.get(base)
                    if role and role[0] == 'authvtable':
                        assign[dst] = ('slot', imm)
                elif ar:
                    dst, base, other = ar.group(1), ar.group(2), ar.group(3)
                    role = roles.get(base)
                    if role and role[0] == 'authvtable' and other in imms:
                        assign[dst] = ('slot', imms[other])
            elif m in INDIRECT:
                reg = ops.split(',')[0].strip()
                role = roles.get(reg)
                if role and role[0] == 'fn':
                    hit = role[1]
            if hit is not None:
                hits.append((insn.address, hit))
                if wanted is not None and hit in wanted:
                    if sum(1 for h in hits if h[1] in wanted) >= limit:
                        return hits, scanned
            for reg in written(insn):
                if reg not in assign:
                    roles.pop(reg, None)
                    imms.pop(reg, None)
            roles.update({k: v for k, v in assign.items() if v})
            imms.update(imm_assign)
    return hits, scanned


def scan_bytes(cache, image, needle, limit=50):
    """Addresses of a byte string inside the image's file-backed segments."""
    if not needle:
        return []
    out = []
    chunk = 4 << 20
    overlap = len(needle) - 1
    for seg in macho.segments(cache, image.address):
        if not seg.filesize:
            continue
        done = 0
        tail = b''
        while done < seg.filesize and len(out) < limit:
            n = min(chunk, seg.filesize - done)
            data = tail + cache.read(seg.vmaddr + done, n)
            base = seg.vmaddr + done - len(tail)
            at = data.find(needle)
            while at >= 0 and len(out) < limit:
                out.append(base + at)
                at = data.find(needle, at + 1)
            tail = data[-overlap:] if overlap else b''
            done += n
    return out
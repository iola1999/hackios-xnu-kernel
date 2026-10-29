#!/usr/bin/env python3
"""Reader for split dyld shared caches: address space, backing files, pointer decoding.

Header field offsets and the pointer rules were measured on the two caches under
ios165/ and ios26/ (both started at 0x180000000):

  - every subcache file carries its own dyld_cache_header and a 32-byte-entry
    mapping table at header+0x10;
  - the main file's image table is at 0x88 (offset) and 0x90 (count), entries are
    32 bytes: uuid[16], address u64, textSegmentSize u32, pathOffset u32;
  - arm64e caches store pointers chained: bit 63 selects the authenticated form
    (low 32 bits are an offset from the shared region start), otherwise the low
    36 bits are the offset;
  - the arm64 cache stores absolute low-36-bit targets with flag bits in 36..43.
"""

import bisect
import glob
import os
import struct

HDR_MAPPING_OFFSET = 0x10
HDR_MAPPING_COUNT = 0x14
HDR_UUID = 0x58
HDR_CACHE_TYPE = 0x68
HDR_IMAGES_OFFSET = 0x88
HDR_IMAGES_COUNT = 0x90
HDR_SHARED_REGION_START = 0xe0
HDR_SHARED_REGION_SIZE = 0xe8

CACHE_PREFIX = 'dyld_shared_cache_'
SKIP_ENDINGS = ('.symbols', '.map', '.atlas', '.json')


class CacheError(Exception):
    pass


class Image:
    __slots__ = ('address', 'text_size', 'path')

    def __init__(self, address, text_size, path):
        self.address = address
        self.text_size = text_size
        self.path = path

    @property
    def name(self):
        return self.path.rsplit('/', 1)[-1]

    def __repr__(self):
        return f'Image({self.address:#x}, {self.path})'


class Mapping:
    __slots__ = ('address', 'size', 'fileoff', 'initprot', 'path')

    def __init__(self, address, size, fileoff, initprot, path):
        self.address = address
        self.size = size
        self.fileoff = fileoff
        self.initprot = initprot
        self.path = path


class Cache:
    def __init__(self, directory):
        self.directory = directory
        if not os.path.isdir(directory):
            raise CacheError(f'cache directory not found: {directory}')
        paths = sorted(p for p in glob.glob(os.path.join(directory, CACHE_PREFIX + '*'))
                       if not p.endswith(SKIP_ENDINGS))
        if not paths:
            raise CacheError(f'no {CACHE_PREFIX}* files in {directory}')
        mains = [p for p in paths if '.' not in os.path.basename(p)]
        if len(mains) != 1:
            raise CacheError(f'expected one main cache file in {directory}, found {len(mains)}')
        self.main_path = mains[0]
        self.files = paths
        self._fds = {}
        self._fd(self.main_path)
        head = self.peek(0, 0x400, self.main_path)
        if head[:7] != b'dyld_v1':
            raise CacheError(f'{self.main_path} does not start with dyld_v1')
        self.magic = head[:16].rstrip(b'\0').decode()
        self.arch = self.magic.split()[-1]
        self.uuid = head[HDR_UUID:HDR_UUID + 16].hex()
        self.cache_type = struct.unpack_from('<Q', head, HDR_CACHE_TYPE)[0]
        self.shared_region_start = struct.unpack_from('<Q', head, HDR_SHARED_REGION_START)[0]
        self.shared_region_size = struct.unpack_from('<Q', head, HDR_SHARED_REGION_SIZE)[0]
        self.images = self._read_images()
        self.mappings = self._read_mappings()
        self._map_starts = [m.address for m in self.mappings]
        self._image_ranges = None

    def _fd(self, path):
        fd = self._fds.get(path)
        if fd is None:
            fd = os.open(path, os.O_RDONLY)
            self._fds[path] = fd
        return fd

    def close(self):
        for fd in self._fds.values():
            os.close(fd)
        self._fds.clear()

    @staticmethod
    def peek(offset, length, path):
        fd = os.open(path, os.O_RDONLY)
        try:
            return os.pread(fd, length, offset)
        finally:
            os.close(fd)

    def _read_images(self):
        head = self.peek(0, 0x100, self.main_path)
        table_off, count = struct.unpack_from('<QQ', head, HDR_IMAGES_OFFSET)
        fd = self._fd(self.main_path)
        table = os.pread(fd, count * 32, table_off)
        images = []
        for i in range(count):
            addr, text_size, path_off = struct.unpack_from('<QII', table, 32 * i + 16)
            raw = os.pread(fd, 512, path_off)
            path = raw.split(b'\0', 1)[0].decode('utf-8', 'replace')
            images.append(Image(addr, text_size, path))
        images.sort(key=lambda img: img.address)
        return images

    def _read_mappings(self):
        mappings = []
        for path in self.files:
            head = self.peek(0, 0x400, path)
            if head[:7] != b'dyld_v1':
                continue
            table_off, count = struct.unpack_from('<II', head, HDR_MAPPING_OFFSET)
            if not count or table_off + count * 32 > os.path.getsize(path):
                continue
            table = self.peek(table_off, count * 32, path)
            for i in range(count):
                addr, size, fileoff, _maxprot, initprot = struct.unpack_from('<QQQII', table, 32 * i)
                mappings.append(Mapping(addr, size, fileoff, initprot, path))
        mappings.sort(key=lambda m: m.address)
        return mappings

    def where(self, addr):
        """(path, file offset, initprot) of the file backing a virtual address."""
        i = bisect.bisect_right(self._map_starts, addr) - 1
        if i < 0:
            raise CacheError(f'address {addr:#x} is below the first mapping')
        m = self.mappings[i]
        if addr >= m.address + m.size:
            raise CacheError(f'address {addr:#x} is not mapped')
        return m.path, m.fileoff + (addr - m.address), m.initprot

    def read(self, addr, length):
        path, off, _prot = self.where(addr)
        data = os.pread(self._fd(path), length, off)
        if len(data) != length:
            raise CacheError(f'short read at {addr:#x}: wanted {length}, got {len(data)}')
        return data

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def u64(self, addr):
        return struct.unpack('<Q', self.read(addr, 8))[0]

    def decode(self, raw):
        """Decode a chained pointer word found in a data segment; None for zero."""
        if raw == 0:
            return None
        if self.arch == 'arm64e':
            if raw >> 63:
                return self.shared_region_start + (raw & 0xffffffff)
            return self.shared_region_start + (raw & ((1 << 36) - 1))
        return raw & ((1 << 36) - 1)

    def find_image(self, name):
        exact = [img for img in self.images if img.path == name or img.path.endswith('/' + name)]
        if len(exact) == 1:
            return exact[0]
        partial = [img for img in self.images if name in img.path]
        if len(partial) == 1:
            return partial[0]
        if not partial:
            raise CacheError(f'no image matching {name!r}')
        listing = '\n'.join(f'  {img.path}' for img in partial[:20])
        raise CacheError(f'{name!r} matches {len(partial)} images:\n{listing}')

    def image_of(self, addr):
        if self._image_ranges is None:
            self._image_ranges = _image_ranges(self)
            self._range_starts = [r[0] for r in self._image_ranges]
        i = bisect.bisect_right(self._range_starts, addr) - 1
        # Segments of different images do not overlap except __LINKEDIT, where the
        # nearest preceding image is the one the address belongs to.
        while i >= 0:
            start, end, image = self._image_ranges[i]
            if start <= addr < end:
                return image
            i -= 1
        return None

    def describe(self, addr):
        """One-line address description: image, segment/section, subcache and offset."""
        import macho
        image = self.image_of(addr)
        path, off, prot = self.where(addr)
        parts = [f'{addr:#x} -> {os.path.basename(path)} fileoff {off:#x} prot {prot:#x}']
        if image is not None:
            parts.append(image.path)
            try:
                for seg in macho.segments(self, image.address):
                    if seg.vmaddr <= addr < seg.vmaddr + max(seg.vmsize, seg.filesize):
                        parts.append(seg.name)
                        for sec in seg.sections:
                            if sec.addr <= addr < sec.addr + sec.size:
                                parts.append(sec.name)
                                break
                        break
            except CacheError:
                pass
        return ' '.join(parts)


def _image_ranges(cache):
    """Sorted (start, end, image) over every segment of every image."""
    import macho
    ranges = []
    for image in cache.images:
        try:
            segs = macho.segments(cache, image.address)
        except (CacheError, struct.error):
            continue
        for seg in segs:
            if seg.vmsize:
                ranges.append((seg.vmaddr, seg.vmaddr + seg.vmsize, image))
    ranges.sort(key=lambda r: r[0])
    return ranges
#!/usr/bin/env python3
"""Vtable candidate scanning, slot decoding, and cross-build slot matching."""

import struct

import disasm
import macho


def _text_range(cache, image):
    for seg in macho.segments(cache, image.address):
        if seg.name == '__TEXT' and seg.filesize:
            return seg.vmaddr, seg.vmaddr + seg.filesize
    raise macho.MachOError(f'{image.path} has no __TEXT segment')


def find_sections(cache, image, section_names):
    out = []
    for seg in macho.segments(cache, image.address):
        for sec in seg.sections:
            if sec.name in section_names:
                out.append((seg, sec))
    return out


def scan(cache, image, section_names=('__const',), min_slots=3):
    """Candidates: 16 zero bytes followed by >= min_slots decoded pointers into __TEXT."""
    lo, hi = _text_range(cache, image)
    hits = []
    for seg, sec in find_sections(cache, image, section_names):
        raw = cache.read(sec.addr, sec.size)
        i = 0
        while i + 24 <= len(raw):
            if raw[i:i + 16] != b'\0' * 16:
                i += 8
                continue
            j = i + 16
            while j + 8 <= len(raw):
                addr = cache.decode(struct.unpack_from('<Q', raw, j)[0])
                if addr is None or not lo <= addr < hi:
                    break
                j += 8
            count = (j - i - 16) // 8
            if count >= min_slots:
                hits.append((sec.addr + i, count, sec.addr + j, seg.name, sec.name))
                i = j
            else:
                i += 8
    return hits


def slots(cache, vtable, count=None, max_count=4096):
    """(index, offset, raw word, decoded address) starting at vtable + 0x10."""
    image = cache.image_of(vtable)
    if image is None:
        raise macho.MachOError(f'no image contains vtable address {vtable:#x}')
    lo, hi = _text_range(cache, image)
    out = []
    while len(out) < (count if count is not None else max_count):
        raw = cache.u64(vtable + 0x10 + 8 * len(out))
        addr = cache.decode(raw)
        if count is None and (addr is None or not lo <= addr < hi):
            break
        out.append((len(out), 8 * len(out), raw, addr))
    return out


def vtable_strings(cache, vtable, count=None, max_insns=120):
    """Per-slot string literals referenced from the start of each slot function."""
    out = []
    for index, offset, _raw, addr in slots(cache, vtable, count=count):
        strings = disasm.function_strings(cache, addr, max_insns) if addr is not None else set()
        out.append((index, offset, addr, strings))
    return out


def match_strings(cache, vtable, count, other_cache, other_vtable, other_count):
    """Slots in two builds whose functions reference a shared string literal."""
    left = vtable_strings(cache, vtable, count)
    right = vtable_strings(other_cache, other_vtable, other_count)
    pairs = []
    for li, loff, laddr, lstr in left:
        for ri, roff, raddr, rstr in right:
            shared = lstr & rstr
            if shared:
                pairs.append((li, ri, loff, roff, laddr, raddr, sorted(shared)))
    return left, right, pairs
#!/usr/bin/env python3
"""dsc.py: read a split dyld shared cache without loading it, and extract images.

  dsc.py [--cache DIR] info
  dsc.py [--cache DIR] images [--filter TEXT] [--limit N]
  dsc.py [--cache DIR] map ADDR [ADDR ...]
  dsc.py [--cache DIR] image NAME
  dsc.py [--cache DIR] extract NAME -o OUT [--no-compact-symbols]
  dsc.py [--cache DIR] vtscan NAME [--section SECT ...] [--min-slots N]
  dsc.py [--cache DIR] slots ADDR [--count N] [--strings [N]]
  dsc.py [--cache DIR] vtmatch ADDR --other-cache DIR --other-vt ADDR
                      [--count N] [--other-count N]
  dsc.py [--cache DIR] read ADDR LENGTH
  dsc.py [--cache DIR] disasm ADDR [--count N] [--stop-on-ret] [--no-annotate]
  dsc.py [--cache DIR] refs NAME TARGET [TARGET ...] [--mode all|call|data] [--limit N]
  dsc.py [--cache DIR] vcalls NAME [--offset OFF ...] [--limit N]
  dsc.py [--cache DIR] findstr NAME TEXT [--limit N]
  dsc.py [--cache DIR] exports NAME [--filter TEXT] [--limit N]

--cache defaults to hackios-xnu-kernel/ios26/23G90__iPhone18,3 relative to the
current directory, then to ios26/23G90__iPhone18,3 next to this tool's parent
hackios-xnu-kernel directory. Addresses accept 0x-prefixed hex or bare hex.
"""

import argparse
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cache as dsc_cache
import disasm
import macho
import vtables

DEFAULT_CACHE = os.path.join('hackios-xnu-kernel', 'ios26', '23G90__iPhone18,3')
TOOL_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def parse_addr(text):
    try:
        return int(text, 0)
    except ValueError:
        return int(text, 16)


def resolve_cache_dir(explicit):
    if explicit:
        return explicit
    candidates = [DEFAULT_CACHE, os.path.join(TOOL_ROOT, 'ios26', '23G90__iPhone18,3')]
    for path in candidates:
        if os.path.isdir(path):
            return path
    raise SystemExit(f'dsc.py: no cache directory found; tried {", ".join(candidates)}; '
                     f'pass --cache DIR')


def add_cache_option(parser, suppress):
    default = argparse.SUPPRESS if suppress else None
    parser.add_argument('--cache', metavar='DIR', default=default,
                        help='split dyld shared cache directory')


def cmd_info(cache, args):
    print(f'directory:     {cache.directory}')
    print(f'magic:         {cache.magic}')
    print(f'arch:          {cache.arch}')
    print(f'uuid:          {cache.uuid}')
    print(f'cache type:    {cache.cache_type}')
    print(f'shared region: {cache.shared_region_start:#x} + {cache.shared_region_size:#x}')
    print(f'files:         {len(cache.files)}')
    print(f'images:        {len(cache.images)}')
    print(f'mappings:      {len(cache.mappings)}')


def cmd_images(cache, args):
    shown = 0
    for image in cache.images:
        if args.filter and args.filter.lower() not in image.path.lower():
            continue
        print(f'{image.address:#014x} text {image.text_size:#010x} {image.path}')
        shown += 1
        if args.limit and shown >= args.limit:
            break


def cmd_map(cache, args):
    for text in args.addr:
        try:
            print(cache.describe(parse_addr(text)))
        except dsc_cache.CacheError as exc:
            print(f'{text}: {exc}')


def cmd_image(cache, args):
    image = cache.find_image(args.name)
    mo = macho.MachO(cache, image.address)
    print(f'image {image.path}')
    print(f'  base {image.address:#x} cputype {mo.cputype:#x} filetype {mo.filetype} '
          f'ncmds {mo.ncmds}')
    for seg in mo.segments:
        host = os.path.basename(cache.where(seg.vmaddr)[0])
        print(f'  segment {seg.name:14s} vm {seg.vmaddr:#014x} vmsize {seg.vmsize:#010x} '
              f'fileoff {seg.fileoff:#010x} filesize {seg.filesize:#010x} '
              f'prot {seg.initprot:#x} host {host}')
        for sec in seg.sections:
            print(f'    section {sec.name:16s} addr {sec.addr:#014x} size {sec.size:#010x} '
                  f'offset {sec.offset:#010x}')


def cmd_extract(cache, args):
    image = cache.find_image(args.name)
    stats = macho.extract(cache, image, args.output,
                          compact_symbols=not args.no_compact_symbols)
    print(f'extracted {image.path} -> {args.output}')
    print(f'  segments copied: {stats["segments"]}')
    print(f'  linkedit blobs:  {stats["linkedit_blobs"]} ({stats["linkedit_bytes"]:#x} bytes)')
    print(f'  symbols:         {stats["symbols"]} (string table {stats["strtab_bytes"]:#x} bytes)')
    print(f'  output size:     {stats["output_bytes"]:#x} bytes')


def cmd_vtscan(cache, args):
    image = cache.find_image(args.name)
    sections = tuple(args.section) if args.section else ('__const',)
    hits = vtables.scan(cache, image, sections, args.min_slots)
    print(f'{image.path}: {len(hits)} candidates')
    for addr, count, end, segname, secname in hits:
        print(f'  {addr:#014x} slots {count:4d} size {end - addr:#07x} end {end:#014x} '
              f'{segname}.{secname}')


def _slot_line(cache, index, offset, raw, addr):
    if addr is None:
        return f'{index:4d} +{offset:#06x} raw {raw:#018x} -'
    image = cache.image_of(addr)
    name = image.name if image else '?'
    return f'{index:4d} +{offset:#06x} raw {raw:#018x} {addr:#014x} {name}'


def cmd_slots(cache, args):
    vtable = parse_addr(args.addr)
    rows = vtables.slots(cache, vtable, count=args.count)
    limit = args.strings
    for index, offset, raw, addr in rows:
        line = _slot_line(cache, index, offset, raw, addr)
        if limit:
            strings = disasm.function_strings(cache, addr, limit) if addr is not None else set()
            if strings:
                line += '  "' + '" "'.join(sorted(strings)) + '"'
        print(line)
    print(f'# {len(rows)} slots at {vtable:#x}')


def cmd_vtmatch(cache, args):
    other = dsc_cache.Cache(args.other_cache)
    vtable = parse_addr(args.vtable)
    other_vtable = parse_addr(args.other_vt)
    count = args.count or len(vtables.slots(cache, vtable))
    other_count = args.other_count or len(vtables.slots(other, other_vtable))
    left, right, pairs = vtables.match_strings(cache, vtable, count,
                                               other, other_vtable, other_count)
    print(f'left  {vtable:#x}: {len(left)} slots; right {other_vtable:#x}: {len(right)} slots')
    print(f'pairs sharing a string literal: {len(pairs)}')
    for li, ri, loff, roff, laddr, raddr, shared in pairs:
        print(f'  idx {li:3d} (+{loff:#06x}) -> {ri:3d} ({roff:#06x})  '
              f'{laddr:#x} / {raddr:#x}  {shared[:2]}')
    deltas = collections.Counter(ri - li for li, ri, *_ in pairs)
    print('index deltas:', ', '.join(f'{d:+d}: {n}' for d, n in deltas.most_common(12)))


def cmd_read(cache, args):
    for line in disasm.hexdump(cache, parse_addr(args.addr), args.length):
        print(line)


def cmd_disasm(cache, args):
    for line in disasm.disasm(cache, parse_addr(args.addr), count=args.count,
                              annotate=not args.no_annotate, stop_on_ret=args.stop_on_ret):
        print(line)


def cmd_refs(cache, args):
    image = cache.find_image(args.name)
    targets = [parse_addr(t) for t in args.target]
    hits, scanned = disasm.scan_refs(cache, image, targets, mode=args.mode, limit=args.limit)
    for addr, kind, target in hits:
        print(f'{addr:#014x} {kind:4s} -> {target:#014x}')
    print(f'# scanned {scanned} instructions, {len(hits)} hits', file=sys.stderr)


def cmd_findstr(cache, args):
    image = cache.find_image(args.name)
    hits = disasm.scan_bytes(cache, image, args.text.encode('utf-8', 'replace'), args.limit)
    for addr in hits:
        print(disasm.describe_target(cache, addr) if args.annotate else f'{addr:#x}')


def cmd_vcalls(cache, args):
    image = cache.find_image(args.name)
    wanted = [parse_addr(o) for o in args.offset] if args.offset else None
    hits, scanned = disasm.scan_virtual_calls(cache, image, wanted, args.limit)
    if wanted:
        hits = [h for h in hits if h[1] in set(wanted)]
    starts = macho.function_starts(cache, image)
    for site, offset in hits:
        i = bisect.bisect_right(starts, site) - 1
        fn = starts[i] if i >= 0 else 0
        print(f'{site:#014x} slot +{offset:#06x} in {fn:#014x} (+{site - fn:#x})')
    print(f'# scanned {scanned} instructions, {len(hits)} calls', file=sys.stderr)


def cmd_exports(cache, args):
    image = cache.find_image(args.name)
    entries = macho.exports(cache, image)
    shown = 0
    for name, kind, value, extra in entries:
        if args.filter and args.filter not in name:
            continue
        print(f'{name} {kind} {value:#x}' + (f' {extra}' if extra else ''))
        shown += 1
        if args.limit and shown >= args.limit:
            break
    print(f'# {len(entries)} exports', file=sys.stderr)


def build_parser():
    parser = argparse.ArgumentParser(
        prog='dsc.py', description='Read a split dyld shared cache and extract Mach-O images.',
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    add_cache_option(parser, suppress=False)
    sub = parser.add_subparsers(dest='command', required=True, metavar='COMMAND')

    def add(name, func, help_text):
        p = sub.add_parser(name, help=help_text)
        add_cache_option(p, suppress=True)
        p.set_defaults(func=func)
        return p

    add('info', cmd_info, 'header fields, image and mapping counts')
    p = add('images', cmd_images, 'list images with load addresses')
    p.add_argument('--filter', help='case-insensitive substring of the path')
    p.add_argument('--limit', type=int, default=0)

    p = add('map', cmd_map, 'show which subcache file backs an address')
    p.add_argument('addr', nargs='+')

    p = add('image', cmd_image, 'segments and sections of one image')
    p.add_argument('name')

    p = add('extract', cmd_extract, 'write one image as a standalone Mach-O')
    p.add_argument('name')
    p.add_argument('-o', '--output', required=True)
    p.add_argument('--no-compact-symbols', action='store_true',
                   help='copy the original symbol and string tables instead of rebuilding them')

    p = add('vtscan', cmd_vtscan, 'find vtable candidates in const sections')
    p.add_argument('name')
    p.add_argument('--section', action='append', default=None,
                   help='section name to scan (default __const, repeatable)')
    p.add_argument('--min-slots', type=int, default=3)

    p = add('slots', cmd_slots, 'decode the slots of a vtable')
    p.add_argument('addr')
    p.add_argument('--count', type=int, default=None,
                   help='slot count (default: until a zero or non-text pointer)')
    p.add_argument('--strings', type=int, nargs='?', const=120, default=0, metavar='INSNS',
                   help='also list string literals referenced by each slot function')

    p = add('vtmatch', cmd_vtmatch, 'match two builds by strings referenced per slot')
    p.add_argument('vtable')
    p.add_argument('--other-cache', required=True, metavar='DIR')
    p.add_argument('--other-vt', required=True)
    p.add_argument('--count', type=int, default=0)
    p.add_argument('--other-count', type=int, default=0)

    p = add('read', cmd_read, 'hex dump')
    p.add_argument('addr')
    p.add_argument('length', type=parse_addr)

    p = add('disasm', cmd_disasm, 'disassemble at an address')
    p.add_argument('addr')
    p.add_argument('--count', type=int, default=40)
    p.add_argument('--stop-on-ret', action='store_true')
    p.add_argument('--no-annotate', action='store_true')

    p = add('refs', cmd_refs, 'find call and data references to addresses in an image')
    p.add_argument('name')
    p.add_argument('target', nargs='+')
    p.add_argument('--mode', choices=('all', 'call', 'data'), default='all')
    p.add_argument('--limit', type=int, default=1000)

    p = add('findstr', cmd_findstr, 'find a byte string inside an image')
    p.add_argument('name')
    p.add_argument('text')
    p.add_argument('--limit', type=int, default=50)
    p.add_argument('--annotate', action='store_true')

    p = add('exports', cmd_exports, 'walk the export trie of an image')
    p.add_argument('name')
    p.add_argument('--filter')
    p.add_argument('--limit', type=int, default=0)

    p = add('vcalls', cmd_vcalls, 'list virtual dispatch sites and their vtable offsets')
    p.add_argument('name')
    p.add_argument('--offset', action='append', default=None,
                   help='only report this slot offset (repeatable)')
    p.add_argument('--limit', type=int, default=1000)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    cache = dsc_cache.Cache(resolve_cache_dir(getattr(args, 'cache', None)))
    try:
        args.func(cache, args)
    except (dsc_cache.CacheError, macho.MachOError) as exc:
        raise SystemExit(f'dsc.py: {exc}')
    finally:
        cache.close()


if __name__ == '__main__':
    main()
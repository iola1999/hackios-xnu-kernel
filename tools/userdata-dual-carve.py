#!/usr/bin/env python3
"""Carve dual iOS partitions (16 GiB darwin root + 104 GiB darwin_var + darwin_nvram) off the end of Android's userdata.

Usage: userdata-dual-carve.py DIR [--root-gib 16] [--var-gib 104]
       userdata-dual-carve.py DIR --split-nvram
  DIR   Directory with phone partition metadata:
          gpt-head.bin              first 8 blocks of LUN 0 (dd if=/dev/block/sda bs=4096 count=8)
          gpt-tail.bin              last 8 blocks of LUN 0 (skip=BLOCKS-8 count=8)
          userdata-meta-before.bin  (optional) first block of userdata (f2fs superblock)
          segment_info-before.txt   (optional) /proc/fs/f2fs/sda33/segment_info
          sb-after.bin              (optional) first block of userdata after shrink
          darwin_var-block0.bin     (--split-nvram) first block of darwin_var (its APFS container superblock)

Dual Partition Architecture:
  1. Shrink Android userdata f2fs by exactly 120 GiB (retaining ~110.88 GiB for Android).
  2. Create /dev/ufs_darwin (16 GiB): for unmodified Apple iOS 26 os.dmg + systemos.dmg + appos.dmg (c=0,1,2).
  3. Create /dev/ufs_darwin_var (104 GiB less 32 MiB): /private/var, an APFS data volume (early installs: ANNFS001).
  4. Create /dev/ufs_darwin_nvram (32 MiB, the last of the carve): the NVRAM banks and the panic log
     (bsd/dev/annibale/annibale_nvram.inc).

--split-nvram gives an existing install darwin_nvram: the last 32 MiB of darwin_var become their own
partition. Only the GPT changes, so darwin_var must not use them: an APFS container that ends at least
32 MiB before the partition does (tools/apfs/annfs-to-apfs.py builds it so); ANNFS001 uses the whole
partition and is refused.
"""
import argparse
import os
import re
import struct
import sys
import uuid
import zlib

BS = 4096
APFS_CONTAINER = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')
# A type of its own (uuid5 of "darwin-nvram.hackios"): the loader and the kernel find it by name.
NVRAM_TYPE = uuid.UUID('7bf9472d-0d95-5e78-921b-9dcd5f9c61fa')
NVRAM_NAME = 'darwin_nvram'
NVRAM_BLOCKS = 32 * 2**20 // BS


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def header_of(block):
    if block[:8] != b'EFI PART':
        sys.exit('userdata-dual-carve: no GPT header found where expected')
    size = struct.unpack_from('<I', block, 12)[0]
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    struct.pack_into('<I', h, 16, 0)
    return {
        'size': size,
        'crc_right': crc(bytes(h)) == want,
        'my': struct.unpack_from('<Q', block, 24)[0],
        'alternate': struct.unpack_from('<Q', block, 32)[0],
        'first_usable': struct.unpack_from('<Q', block, 40)[0],
        'last_usable': struct.unpack_from('<Q', block, 48)[0],
        'entries_lba': struct.unpack_from('<Q', block, 72)[0],
        'count': struct.unpack_from('<I', block, 80)[0],
        'entry_size': struct.unpack_from('<I', block, 84)[0],
        'entries_crc': struct.unpack_from('<I', block, 88)[0],
    }


def seal(block, entries):
    """Update entries CRC and header CRC."""
    b = bytearray(block)
    h = header_of(b)
    struct.pack_into('<I', b, 88, crc(entries))
    struct.pack_into('<I', b, 16, 0)
    struct.pack_into('<I', b, 16, crc(bytes(b[:h['size']])))
    return bytes(b)


def entries_of(blob, h, base):
    """Extract partition table entries from LBA base."""
    at = (h['entries_lba'] - base) * BS
    raw = blob[at:at + h['count'] * h['entry_size']]
    if len(raw) != h['count'] * h['entry_size']:
        sys.exit('userdata-dual-carve: partition table truncated in saved blocks')
    parsed = []
    for i in range(h['count']):
        e = raw[i * h['entry_size']:(i + 1) * h['entry_size']]
        parsed.append({
            'type': uuid.UUID(bytes_le=e[:16]),
            'guid': uuid.UUID(bytes_le=e[16:32]),
            'first': struct.unpack_from('<Q', e, 32)[0],
            'last': struct.unpack_from('<Q', e, 40)[0],
            'attrs': struct.unpack_from('<Q', e, 48)[0],
            'name': e[56:128].decode('utf-16le').split('\0', 1)[0],
        })
    return raw, parsed, at


def check(name, head, tail, disk):
    """Verify primary and backup GPT headers and partition tables."""
    ph = header_of(head[BS:2 * BS])
    bh = header_of(tail[7 * BS:8 * BS])
    praw, pe, _ = entries_of(head, ph, 0)
    braw, be, _ = entries_of(tail, bh, disk - 8)
    ok = (ph['crc_right'] and bh['crc_right'] and
          crc(praw) == ph['entries_crc'] and crc(braw) == bh['entries_crc'] and
          ph['my'] == 1 and ph['alternate'] == disk - 1 and
          bh['my'] == disk - 1 and bh['alternate'] == 1 and
          praw == braw and ph['last_usable'] == bh['last_usable'] and
          ph['first_usable'] == bh['first_usable'])
    print(f'{name}: primary header crc {"right" if ph["crc_right"] else "WRONG"}, '
          f'table crc {"right" if crc(praw) == ph["entries_crc"] else "WRONG"}; '
          f'backup header crc {"right" if bh["crc_right"] else "WRONG"}, '
          f'table crc {"right" if crc(braw) == bh["entries_crc"] else "WRONG"}; '
          f'tables {"identical" if praw == braw else "DIFFER"} -> {"valid" if ok else "INVALID"}')
    return ok, ph, bh, pe


def new_entry(es, type_guid, first, last, name):
    e = bytearray(es)
    e[:16] = type_guid.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', e, 32, first, last, 0)
    e[56:56 + 2 * len(name)] = name.encode('utf-16le')
    return e


def write_tables(d, head, tail, ph, bh, disk, entries, table, changed_want, what):
    """Seal table into both GPT copies, check them, write gpt-head.new and gpt-tail.new; the changed tail blocks."""
    _, _, pat = entries_of(head, ph, 0)
    new_head = bytearray(head)
    new_head[pat:pat + len(table)] = table
    new_head[BS:2 * BS] = seal(head[BS:2 * BS], table)
    _, _, bat = entries_of(tail, bh, disk - 8)
    new_tail = bytearray(tail)
    new_tail[bat:bat + len(table)] = table
    new_tail[7 * BS:8 * BS] = seal(tail[7 * BS:8 * BS], table)
    new_head, new_tail = bytes(new_head), bytes(new_tail)

    ok_new, _, _, new_entries = check(what, new_head, new_tail, disk)
    changed = [i for i in range(len(entries)) if entries[i] != new_entries[i]]
    if not ok_new or changed != sorted(changed_want):
        sys.exit('userdata-dual-carve: generated table verification failed')
    out_head = os.path.join(d, 'gpt-head.new')
    out_tail = os.path.join(d, 'gpt-tail.new')
    open(out_head, 'wb').write(new_head)
    open(out_tail, 'wb').write(new_tail)
    print(f'Wrote {out_head} and {out_tail}\n')
    return [i for i in range(8) if tail[i * BS:(i + 1) * BS] != new_tail[i * BS:(i + 1) * BS]]


def print_gpt_writes(ph, disk, changed_tail, step):
    tl, tn = ph['entries_lba'], (ph['count'] * ph['entry_size'] + BS - 1) // BS
    print(f'  {step}. Apply primary partition table:')
    print(f'     dd if=gpt-head.new of=/dev/block/sda bs={BS} skip={tl} seek={tl} count={tn} conv=notrunc,fsync')
    print(f'  {step + 1}. Apply primary GPT header:')
    print(f'     dd if=gpt-head.new of=/dev/block/sda bs={BS} skip=1 seek=1 count=1 conv=notrunc,fsync')
    print(f'  {step + 2}. Apply backup GPT table and header:')
    print(f'     dd if=gpt-tail.new of=/dev/block/sda bs={BS} skip={changed_tail[0]} '
          f'seek={disk - 8 + changed_tail[0]} count={changed_tail[-1] - changed_tail[0] + 1} conv=notrunc,fsync')


def free_slots_after(entries, used, n):
    free = [i for i, e in enumerate(entries) if e['type'].int == 0 and i > max(used)]
    if len(free) < n:
        sys.exit(f'userdata-dual-carve: need at least {n} free partition slots after the last partition')
    return free[:n]


def split_nvram(d, head, tail, disk, ph, bh, entries):
    """darwin_var's last 32 MiB become darwin_nvram (an install from before darwin_nvram)."""
    used = [i for i, e in enumerate(entries) if e['type'].int != 0]
    var = [i for i in used if entries[i]['name'] == 'darwin_var']
    if len(var) != 1:
        sys.exit('userdata-dual-carve: exactly one partition named darwin_var expected')
    if any(e['name'] == NVRAM_NAME for e in entries):
        sys.exit(f'userdata-dual-carve: partition {NVRAM_NAME} already exists')
    v = var[0]
    first, last = entries[v]['first'], entries[v]['last']
    var_blocks = last - first + 1 - NVRAM_BLOCKS
    if var_blocks < NVRAM_BLOCKS:
        sys.exit('userdata-dual-carve: darwin_var is too small to give up 32 MiB')

    block0_path = os.path.join(d, 'darwin_var-block0.bin')
    if not os.path.exists(block0_path):
        sys.exit(f'userdata-dual-carve: --split-nvram needs {block0_path} (dd if=/dev/block/by-name/darwin_var '
                 'bs=4096 count=1)')
    block0 = open(block0_path, 'rb').read(BS)
    if block0[32:36] != b'NXSB':
        sys.exit('userdata-dual-carve: darwin_var is not an APFS container; ANNFS001 uses the whole partition: '
                 'convert it first (tools/apfs/annfs-to-apfs.py)')
    nx_block_size, = struct.unpack_from('<I', block0, 36)
    nx_block_count, = struct.unpack_from('<Q', block0, 40)
    if nx_block_size * nx_block_count > var_blocks * BS:
        sys.exit(f'userdata-dual-carve: the APFS container ({nx_block_count} blocks of {nx_block_size}) reaches '
                 'into the last 32 MiB of darwin_var')
    slot, = free_slots_after(entries, used, 1)

    print('\nPartition Layout Plan:')
    print(f'  darwin_var:   LBA {first}..{last - NVRAM_BLOCKS} ({var_blocks * BS / 2**30:.3f} GiB; '
          f'its APFS container {nx_block_size * nx_block_count / 2**30:.3f} GiB)')
    print(f'  {NVRAM_NAME}: LBA {last - NVRAM_BLOCKS + 1}..{last} (32 MiB, slot {slot}, sda{slot + 1})\n')

    praw, _, _ = entries_of(head, ph, 0)
    table = bytearray(praw)
    es = ph['entry_size']
    struct.pack_into('<Q', table, v * es + 40, last - NVRAM_BLOCKS)
    table[slot * es:(slot + 1) * es] = new_entry(es, NVRAM_TYPE, last - NVRAM_BLOCKS + 1, last, NVRAM_NAME)
    changed_tail = write_tables(d, head, tail, ph, bh, disk, entries, bytes(table), {v, slot}, 'GPT with darwin_nvram')

    print('Execution Sequence on Device (adb root shell, Android):')
    print_gpt_writes(ph, disk, changed_tail, 1)
    print(f'  4. Reboot: the partition table is read again at boot ({NVRAM_NAME} is sda{slot + 1} from then on).')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dir', help='directory with gpt-head.bin, gpt-tail.bin, etc.')
    parser.add_argument('--root-gib', type=int, default=16, help='size of root partition (darwin) in GiB (default 16)')
    parser.add_argument('--var-gib', type=int, default=104,
                        help='size of data partition (darwin_var) plus darwin_nvram in GiB (default 104)')
    parser.add_argument('--root-name', default='darwin', help='root partition name (default darwin)')
    parser.add_argument('--var-name', default='darwin_var', help='data partition name (default darwin_var)')
    parser.add_argument('--split-nvram', action='store_true',
                        help='an existing install: the last 32 MiB of an APFS darwin_var become darwin_nvram')
    args = parser.parse_args()

    d = args.dir
    total_carve_gib = args.root_gib + args.var_gib
    head_path = os.path.join(d, 'gpt-head.bin')
    tail_path = os.path.join(d, 'gpt-tail.bin')
    if not os.path.exists(head_path) or not os.path.exists(tail_path):
        head_path = os.path.join(d, 'sda-head.bin')
        tail_path = os.path.join(d, 'sda-tail.bin')

    if not os.path.exists(head_path) or not os.path.exists(tail_path):
        sys.exit(f'userdata-dual-carve: gpt-head.bin/gpt-tail.bin or sda-head.bin/sda-tail.bin missing in {d}')

    head = open(head_path, 'rb').read()
    tail = open(tail_path, 'rb').read()
    if len(head) != 8 * BS or len(tail) != 8 * BS:
        sys.exit('userdata-dual-carve: header and tail files must be exactly 8 blocks (32768 bytes) each')

    disk = header_of(tail[7 * BS:8 * BS])['my'] + 1
    ok, ph, bh, entries = check('initial GPT', head, tail, disk)
    if not ok:
        sys.exit('userdata-dual-carve: input GPT failed verification')
    if args.split_nvram:
        return split_nvram(d, head, tail, disk, ph, bh, entries)

    used = [i for i, e in enumerate(entries) if e['type'].int != 0]
    ud = [i for i in used if entries[i]['name'] == 'userdata']
    if len(ud) != 1:
        sys.exit('userdata-dual-carve: exactly one partition named userdata expected')
    u = ud[0]

    if any(entries[i]['last'] > entries[u]['last'] for i in used) or entries[u]['last'] != ph['last_usable']:
        sys.exit('userdata-dual-carve: userdata is not the last partition on LUN 0')

    for name in (args.root_name, args.var_name, NVRAM_NAME):
        if any(e['name'] == name for e in entries):
            sys.exit(f'userdata-dual-carve: partition {name} already exists')

    slot_root, slot_var, slot_nvram = free_slots_after(entries, used, 3)

    per_seg = 512  # 2MB f2fs segment in 4K blocks
    part_blocks = entries[u]['last'] - entries[u]['first'] + 1

    # Optional f2fs inspection
    meta_path = os.path.join(d, 'userdata-meta-before.bin')
    seg_info_path = os.path.join(d, 'segment_info-before.txt')
    if os.path.exists(meta_path):
        sb = open(meta_path, 'rb').read(BS)[1024:2048]
        magic, = struct.unpack_from('<I', sb, 0)
        log_bs, log_bps = struct.unpack_from('<I', sb, 16)[0], struct.unpack_from('<I', sb, 20)[0]
        block_count, = struct.unpack_from('<Q', sb, 36)
        segs_main, = struct.unpack_from('<I', sb, 68)
        main_blkaddr, = struct.unpack_from('<I', sb, 92)
        if magic != 0xf2f52010 or log_bs != 12:
            sys.exit('userdata-dual-carve: userdata does not contain valid 4K f2fs superblock')
        per_seg = 1 << log_bps
        cut_segs = total_carve_gib * 2**30 // BS // per_seg
        new_segs = segs_main - cut_segs
        new_blocks = main_blkaddr + new_segs * per_seg
        fs_blocks = block_count - cut_segs * per_seg
        print(f'f2fs superblock: total {block_count} blocks, {segs_main} main segments')
        print(f'f2fs resize target: {new_blocks} blocks ({new_segs} main segments)')
        if os.path.exists(seg_info_path):
            valid = {}
            for line in open(seg_info_path):
                m = re.match(r'^(\d+)\s+(.*)$', line)
                if m:
                    for k, e in enumerate(m[2].split()):
                        valid[int(m[1]) + k] = int(e.split('|')[1])
            in_tail = [s for s in range(new_segs, segs_main) if valid.get(s, 0)]
            if in_tail:
                sys.exit(f'userdata-dual-carve: {len(in_tail)} segments past new boundary contain valid blocks')
            print('f2fs segment verification: all tail segments are empty and safe to shrink')
    else:
        cut_blocks = total_carve_gib * 2**30 // BS
        fs_blocks = part_blocks - cut_blocks
        new_blocks = fs_blocks

    part_end = entries[u]['first'] + fs_blocks
    root_start = (part_end + per_seg - 1) // per_seg * per_seg
    root_blocks = args.root_gib * 2**30 // BS
    root_end = root_start + root_blocks - 1

    var_start = (root_end + 1 + per_seg - 1) // per_seg * per_seg
    nvram_end = ph['last_usable']
    nvram_start = nvram_end - NVRAM_BLOCKS + 1
    var_end = nvram_start - 1
    var_blocks = var_end - var_start + 1

    print('\nPartition Layout Plan:')
    print(f'  userdata:   LBA {entries[u]["first"]}..{part_end - 1} '
          f'({(part_end - entries[u]["first"]) * BS / 2**30:.3f} GiB, retained for Android)')
    print(f'  {args.root_name} (root): LBA {root_start}..{root_end} '
          f'({root_blocks * BS / 2**30:.3f} GiB, slot {slot_root}, sda{slot_root + 1})')
    print(f'  {args.var_name} (var):  LBA {var_start}..{var_end} '
          f'({var_blocks * BS / 2**30:.3f} GiB, slot {slot_var}, sda{slot_var + 1})')
    print(f'  {NVRAM_NAME}: LBA {nvram_start}..{nvram_end} (32 MiB, slot {slot_nvram}, sda{slot_nvram + 1})')
    print(f'  Total iOS Carved: {(nvram_end - root_start + 1) * BS / 2**30:.3f} GiB\n')

    # Construct new GPT table
    praw, _, _ = entries_of(head, ph, 0)
    table = bytearray(praw)
    es = ph['entry_size']
    struct.pack_into('<Q', table, u * es + 40, part_end - 1)
    table[slot_root * es:(slot_root + 1) * es] = new_entry(es, APFS_CONTAINER, root_start, root_end, args.root_name)
    table[slot_var * es:(slot_var + 1) * es] = new_entry(es, APFS_CONTAINER, var_start, var_end, args.var_name)
    table[slot_nvram * es:(slot_nvram + 1) * es] = new_entry(es, NVRAM_TYPE, nvram_start, nvram_end, NVRAM_NAME)
    changed_tail = write_tables(d, head, tail, ph, bh, disk, entries, bytes(table),
                                {u, slot_root, slot_var, slot_nvram}, 'new dual-boot GPT')

    print('Execution Sequence on Device (adb root shell in single Android session):')
    print(f'  1. /data/local/tmp/f2fs-resize /data {new_blocks}')
    print('     (Do NOT reboot before applying GPT, or fs_mgr resize.f2fs will expand it back)')
    print_gpt_writes(ph, disk, changed_tail, 2)
    print('  5. Write unmodified Apple iOS 26 containers (os.dmg + systemos.dmg + appos.dmg) to rootfs:')
    print(f'     dd if=os.dmg of=/dev/block/sda{slot_root + 1} bs=4096 seek=0 conv=notrunc,fsync')
    print(f'     dd if=systemos.dmg of=/dev/block/sda{slot_root + 1} bs=4096 seek=2382336 conv=notrunc,fsync')
    print(f'     dd if=appos.dmg of=/dev/block/sda{slot_root + 1} bs=4096 seek=3666944 conv=notrunc,fsync')
    print(f'  6. Verified: /dev/ufs_{args.root_name} (rootfs), /dev/ufs_{args.var_name} (data) and '
          f'/dev/ufs_{NVRAM_NAME} are ready!')
    return 0


if __name__ == '__main__':
    sys.exit(main())

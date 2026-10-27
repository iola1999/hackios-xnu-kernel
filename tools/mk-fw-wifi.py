#!/usr/bin/env python3
"""
mk-fw-wifi.py: the Wi-Fi firmware the phone's own modem partition carries, and the board data
ath12k asks for made out of it. Nothing of the firmware is in the repository.

The modem partition (what Android mounts as /vendor/firmware_mnt) holds the WCN7850 set under
image/kiwi/: the main firmware (amss.bin, amss20.bin for hardware 2.0), the PHY microcode, the
regulatory database, and 63 board data files (bdwlan.e01 ... e41, e52, bdwlan.elf). ath12k wants
the firmware from its firmware directory (WCN7850/hw2.0) and the board data in a container it
calls board-2.bin, which this script also builds. Which of those 63 files is this phone's is
answered by the board id the firmware reports once the download works (ath12k's QMI target
info), so the container is built again then.

  mk-fw-wifi.py list IMAGE
      what image/kiwi holds: name and size of each entry. IMAGE is a .img or .img.gz of the
      modem partition; it is read with gzip and the FAT16 reader below, nothing is unpacked.

  mk-fw-wifi.py extract IMAGE DST
      writes those entries to DST and DST/manifest.json: name, size, SHA-256 and the image the
      file came from. Run it on the phone's own partition rather than trusting a version from
      the internet: the amss the phone was calibrated with is the one to boot.

  mk-fw-wifi.py board2 BDF DST --subsystem-vendor V --subsystem-device D [--board-id N]
                [--variant S] [--regdb FILE] [--chip-id N]
      board-2.bin for ath12k's firmware API 2, holding that board data file under the name the
      driver builds from the PCI identities and from QMI's target info:
      "bus=pci,vendor=17cb,device=1107,subsystem-vendor=..,subsystem-device=..,
      qmi-chip-id=..,qmi-board-id=..[,variant=..]". A second entry with board id 255 follows,
      which is the name the driver falls back to when the firmware reports no board id.
      --regdb puts the regulatory database into the same file under the same name and under
      "bus=pci", where the driver looks for it after the board data.

The container's layout is ath12k's, from its sources (drivers/net/wireless/ath/ath12k): the
17-byte magic "QCA-ATH12K-BOARD" with a null byte, padded to 4; then IEs of {u32 id, u32 len,
data padded to 4}, id 0 holding board data and id 1 holding the regulatory database; inside one
of those, id 0 holds the name and id 1 the file itself. The chip id and the shape of the name in
`board2` come from linux-firmware's own ath12k/WCN7850/hw2.0/board-2.bin: every entry there is
qmi-chip-id=2, and the board data files it carries are the same size as the ones in image/kiwi.
`board2` reads back what it wrote before saying it is done.
"""
import argparse
import gzip
import hashlib
import json
import os
import struct
import sys

BOARD_MAGIC = b'QCA-ATH12K-BOARD\0'
IE_BOARD = 0
IE_REGDB = 1
IE_NAME = 0
IE_DATA = 1
BOARD_ID_DEFAULT = 0xff
WCN7850_VENDOR = 0x17cb
WCN7850_DEVICE = 0x1107
WCN7850_CHIP_ID = 2
KIWI_DIR = 'image/kiwi'


# ---- the FAT16 the modem partition is formatted with ----------------------------------------

class Fat16:
    """Just enough of FAT16 to walk image/kiwi: fixed-size root directory, files by cluster chain."""

    def __init__(self, image):
        self.image = image
        bytes_per_sector, = struct.unpack_from('<H', image, 11)
        sectors_per_cluster = image[13]
        reserved, = struct.unpack_from('<H', image, 14)
        fats = image[16]
        root_entries, = struct.unpack_from('<H', image, 17)
        fat_size, = struct.unpack_from('<H', image, 22)
        self.cluster_bytes = bytes_per_sector * sectors_per_cluster
        self.fat_start = reserved * bytes_per_sector
        self.fat_bytes = fat_size * bytes_per_sector
        self.root_start = self.fat_start + fats * self.fat_bytes
        self.root_bytes = root_entries * 32
        self.data_start = self.root_start + self.root_bytes

    def next_cluster(self, cluster):
        value, = struct.unpack_from('<H', self.image, self.fat_start + 2 * cluster)
        return None if value >= 0xfff8 or value == 0 else value

    def read_chain(self, first, size=None):
        """The bytes of a cluster chain; a directory is read whole, a file up to its size."""
        data = bytearray()
        cluster = first
        while cluster is not None:
            offset = self.data_start + (cluster - 2) * self.cluster_bytes
            data += self.image[offset:offset + self.cluster_bytes]
            cluster = self.next_cluster(cluster)
        return bytes(data if size is None else data[:size])

    def entries(self, directory):
        """(long or short name, short name, is_directory, first_cluster, size) per entry.

        The image was written with long names, so the 8.3 name of a file often says nothing
        ("PCAFUI~2" for "image"); each long name is put back together from the entries in front
        of its short one, in the order those carry.
        """
        out, long_name = [], {}
        for offset in range(0, len(directory), 32):
            entry = directory[offset:offset + 32]
            if len(entry) < 32 or entry[0] == 0x00:
                break
            if entry[0] == 0xe5:
                long_name = {}
                continue
            if entry[11] & 0x0f == 0x0f:  # one piece of a long name
                piece = entry[1:11] + entry[14:26] + entry[28:32]
                long_name[entry[0] & 0x3f] = piece.decode('utf-16-le', 'replace')
                continue
            first, = struct.unpack_from('<H', entry, 26)
            size, = struct.unpack_from('<I', entry, 28)
            base = entry[0:8].decode('ascii', 'replace').rstrip()
            extension = entry[8:11].decode('ascii', 'replace').rstrip()
            short = base + ('.' + extension if extension else '')
            name = ''.join(long_name[order] for order in sorted(long_name)) if long_name else short
            name = name.split('\0')[0] or short
            out.append((name, short, bool(entry[11] & 0x10), first, size))
            long_name = {}
        return out

    def ls(self, path):
        """The entries of a directory given as a path inside the image."""
        directory = self.image[self.root_start:self.root_start + self.root_bytes]
        for part in path.strip('/').split('/'):
            found = [e for e in self.entries(directory)
                     if part.lower() in (e[0].lower(), e[1].lower())]
            if not found or not found[0][2]:
                raise SystemExit('mk-fw-wifi.py: %s is not in the image' % path)
            directory = self.read_chain(found[0][3])
        return self.entries(directory)


def open_image(path):
    with open(path, 'rb') as handle:
        raw = handle.read()
    return gzip.decompress(raw) if path.endswith('.gz') else raw


def kiwi_files(image_path):
    fat = Fat16(open_image(image_path))
    files = []
    for name, short, is_directory, first, size in fat.ls(KIWI_DIR):
        if not is_directory:
            files.append((name, fat.read_chain(first, size)))
    return sorted(files, key=lambda f: f[0])


# ---- board-2.bin ---------------------------------------------------------------------------

def ie(ie_id, payload):
    return struct.pack('<II', ie_id, len(payload)) + payload + b'\0' * (-len(payload) % 4)


def board_name(chip_id, board_id, identities, variant=''):
    vendor, device, subsystem_vendor, subsystem_device = identities
    name = ('bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,subsystem-device=%04x,'
            'qmi-chip-id=%d,qmi-board-id=%d' % (vendor, device, subsystem_vendor,
                                               subsystem_device, chip_id, board_id))
    return name + (',variant=' + variant if variant else '')


def board2(data, names, regdb=None):
    out = BOARD_MAGIC + b'\0' * (-len(BOARD_MAGIC) % 4)
    for name in names:
        out += ie(IE_BOARD, ie(IE_NAME, name.encode()) + ie(IE_DATA, data))
    if regdb is not None:
        for name in (names[0], 'bus=pci'):
            out += ie(IE_REGDB, ie(IE_NAME, name.encode()) + ie(IE_DATA, regdb))
    return out


def check_board2(blob, name, data, regdb=None):
    """Read the file back the way ath12k reads it: the magic, then IEs of {id, len, data}; the
    board data is the id 1 sub-IE that follows an id 0 sub-IE holding the wanted name."""
    if blob[:len(BOARD_MAGIC)] != BOARD_MAGIC:
        raise SystemExit('mk-fw-wifi.py: the magic did not read back')
    offset = len(BOARD_MAGIC) + (-len(BOARD_MAGIC) % 4)
    found = {IE_BOARD: None, IE_REGDB: None}
    while offset + 8 <= len(blob):
        ie_id, length = struct.unpack_from('<II', blob, offset)
        body = blob[offset + 8:offset + 8 + length]
        sub, at = {}, 0
        while at + 8 <= len(body):
            sub_id, sub_len = struct.unpack_from('<II', body, at)
            sub.setdefault(sub_id, body[at + 8:at + 8 + sub_len])
            at += 8 + sub_len + (-sub_len % 4)
        if sub.get(IE_NAME) == name.encode():
            found[ie_id] = sub.get(IE_DATA)
        offset += 8 + length + (-length % 4)
    if found[IE_BOARD] != data:
        raise SystemExit('mk-fw-wifi.py: board data did not read back under %s' % name)
    if regdb is not None and found[IE_REGDB] != regdb:
        raise SystemExit('mk-fw-wifi.py: regdb did not read back')


# ---- command line -------------------------------------------------------------------------

def number(text):
    return int(text, 0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)

    listing = commands.add_parser('list', help='what image/kiwi holds')
    listing.add_argument('image')

    extract = commands.add_parser('extract', help='write image/kiwi out with a manifest')
    extract.add_argument('image')
    extract.add_argument('destination')

    package = commands.add_parser('board2', help='board-2.bin for ath12k from one bdf file')
    package.add_argument('bdf')
    package.add_argument('destination')
    package.add_argument('--subsystem-vendor', type=number, required=True)
    package.add_argument('--subsystem-device', type=number, required=True)
    package.add_argument('--chip-id', type=number, default=WCN7850_CHIP_ID)
    package.add_argument('--board-id', type=number, default=BOARD_ID_DEFAULT)
    package.add_argument('--variant', default='')
    package.add_argument('--regdb')
    package.add_argument('--vendor', type=number, default=WCN7850_VENDOR)
    package.add_argument('--device', type=number, default=WCN7850_DEVICE)

    args = parser.parse_args(argv[1:])

    if args.command == 'list':
        for name, data in kiwi_files(args.image):
            print('%10d  %s' % (len(data), name))
        return 0

    if args.command == 'extract':
        os.makedirs(args.destination, exist_ok=True)
        manifest = {'image': os.path.basename(args.image), 'files': []}
        for name, data in kiwi_files(args.image):
            with open(os.path.join(args.destination, name), 'wb') as handle:
                handle.write(data)
            manifest['files'].append({'name': name, 'size': len(data),
                                      'sha256': hashlib.sha256(data).hexdigest()})
            print('%10d  %s' % (len(data), name))
        with open(os.path.join(args.destination, 'manifest.json'), 'w') as handle:
            json.dump(manifest, handle, indent=1, sort_keys=True)
            handle.write('\n')
        print('%d files and manifest.json in %s' % (len(manifest['files']), args.destination))
        return 0

    identities = (args.vendor, args.device, args.subsystem_vendor, args.subsystem_device)
    with open(args.bdf, 'rb') as handle:
        data = handle.read()
    regdb = None
    if args.regdb is not None:
        with open(args.regdb, 'rb') as handle:
            regdb = handle.read()
    names = [board_name(args.chip_id, args.board_id, identities, args.variant),
             board_name(args.chip_id, BOARD_ID_DEFAULT, identities)]
    blob = board2(data, names, regdb)
    check_board2(blob, names[0], data, regdb)
    with open(args.destination, 'wb') as handle:
        handle.write(blob)
    print('%d bytes in %s' % (len(blob), args.destination))
    for name in names:
        print('board data %d bytes as %s' % (len(data), name))
    if regdb is not None:
        print('regdb %d bytes as %s and bus=pci' % (len(regdb), names[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
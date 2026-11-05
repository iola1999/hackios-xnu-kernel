#!/usr/bin/env python3
"""
mk-fw-bt.py: the Bluetooth firmware the phone's own bluetooth partition carries, for the kernel's bring-up over SE14
(boot-arg bt=3, xnu/files/bsd/dev/annibale_bt_hci.c). Nothing of the firmware is in the repository.

The partition (bluetooth_a; Android mounts it as /vendor/bt_firmware) is FAT16 with everything under image/: the
files of several Qualcomm chips (brh*, gng* and the UWB parts) and the WCN7850's, which Linux's btqca.c names
"hmt": the rampatch hmtbtfw20.tlv with its version string hmtbtfw20.ver, and the NVM hmtnv20.bin with one variant
per board id (hmtnv20.b<id>). The kernel reads the ROM version and the board id from the chip and picks the files
by those, so all hmt* files are taken.

  mk-fw-bt.py list IMAGE
      name and size of each entry of image/. IMAGE is a .img or .img.gz of the partition, read in memory.

  mk-fw-bt.py extract IMAGE [DST]
      writes the hmt* entries to DST (default hackios-xnu-kernel/out/fw-bt, where tools/k90auto/package.py BT_FW=1
      takes them from) and DST/manifest.json: name, size and SHA-256 of each, and the image's file name.

The FAT16 reader is mk-fw-wifi.py's.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DST = os.path.normpath(os.path.join(HERE, '..', 'out', 'fw-bt'))
IMAGE_DIR = 'image'
PREFIX = 'hmt'


def fat_reader():
    spec = importlib.util.spec_from_file_location('mk_fw_wifi', os.path.join(HERE, 'mk-fw-wifi.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def entries(image_path):
    wifi = fat_reader()
    fat = wifi.Fat16(wifi.open_image(image_path))
    for name, short, is_directory, first, size in fat.ls(IMAGE_DIR):
        if not is_directory:
            yield name, size, lambda first=first, size=size: fat.read_chain(first, size)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    listing = commands.add_parser('list', help='what image/ holds')
    listing.add_argument('image')
    extract = commands.add_parser('extract', help='write the hmt* files out with a manifest')
    extract.add_argument('image')
    extract.add_argument('destination', nargs='?', default=DEFAULT_DST)
    args = parser.parse_args(argv[1:])

    if args.command == 'list':
        for name, size, _ in sorted(entries(args.image)):
            print('%8d  %s' % (size, name))
        return 0

    os.makedirs(args.destination, exist_ok=True)
    manifest = {'image': os.path.basename(args.image), 'files': []}
    for name, size, read in sorted(entries(args.image)):
        if not name.startswith(PREFIX):
            continue
        data = read()
        with open(os.path.join(args.destination, name), 'wb') as handle:
            handle.write(data)
        manifest['files'].append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    if not any(f['name'].endswith('.tlv') for f in manifest['files']):
        raise SystemExit('mk-fw-bt.py: no %s*.tlv rampatch in %s/ of the image' % (PREFIX, IMAGE_DIR))
    with open(os.path.join(args.destination, 'manifest.json'), 'w') as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True)
    print('%d files, %d bytes, and manifest.json in %s' % (len(manifest['files']),
                                                          sum(f['size'] for f in manifest['files']), args.destination))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

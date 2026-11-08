#!/usr/bin/env python3
"""Re-seal a loader boot image around a new XNUP container.

usage: reseal-loader-img.py REFERENCE.img PAYLOAD.bin -o OUT.img

The slot-booted loader images (handoff-loader-v6 and later) are an Android boot image, header
version 1, whose kernel is gzip(BootShim + UEFI FD + XNUP container) followed by the merged
DTB. Everything except the container is taken from REFERENCE byte for byte: the legacy 176-byte
BootShim, the FD, the DTB (an image with any other tail returns to LK about 7 s after the jump)
and the ramdisk bytes. mkxnuimg.py builds the
shim from the current BootShim.S instead, which is a different shim.

The container is padded to 16 bytes: the legacy shim copies its size rounded up to 16. The gzip
stream is zlib's own wrapper at level 9, which is what reproduces the v8 stream exactly. v8's own
container (89,224 bytes) went in unpadded; the same steps without the padding give the v8 file
back except the 20-byte header id, which v8 carries stale and LK does not check. This tool writes
the id mkbootimg computes.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# AOSP's mkbootimg, the same file as Mu-Silicium's Resources/Scripts/mkbootimg.py
MKBOOTIMG = os.path.join(HERE, '..', 'prebuilt', 'uefi', 'mkbootimg.py')


def validate_container(container):
    if len(container) < 32 or container[:4] != b'XNUP':
        raise ValueError('payload: not an XNUP container')
    if struct.unpack_from('<I', container, 4)[0] != len(container) - 8:
        raise ValueError('payload: declared length differs from file length')
    at = 8
    while at + 24 <= len(container):
        header = container[at:at + 24]
        name, size, reserved = struct.unpack('<16sII', header)
        at += 24
        if name[0] == 0:
            if header != bytes(24) or at != len(container):
                raise ValueError('payload: invalid terminator')
            return
        if b'\0' not in name or reserved:
            raise ValueError('payload: invalid entry header')
        name = name.split(b'\0', 1)[0]
        if name == b'payload.lz4':
            # The loader consumes kernel/ramdisk entries directly; it does not unpack this entry.
            raise ValueError('payload: payload.lz4 is unsupported; pass the uncompressed standalone XNUP')
        end = at + size
        at += (size + 15) & ~15
        if at > len(container) or any(container[end:at]):
            raise ValueError('payload: truncated entry or nonzero padding')
    raise ValueError('payload: missing terminator')


def split(image):
    if image[:8] != b'ANDROID!':
        sys.exit('reference: not an Android boot image')
    ksz, rsz = struct.unpack('<I', image[8:12])[0], struct.unpack('<I', image[16:20])[0]
    page = struct.unpack('<I', image[0x24:0x28])[0]
    stream = zlib.decompressobj(31)
    blob = stream.decompress(image[page:page + ksz])
    roff = page + (ksz + page - 1) // page * page
    at = blob.find(b'XNUP')
    if at < 0:
        sys.exit('reference: no XNUP container in the kernel blob')
    return blob[:at], stream.unused_data, image[roff:roff + rsz]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('reference')
    ap.add_argument('payload')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    prefix, dtb, ramdisk = split(open(args.reference, 'rb').read())
    container = open(args.payload, 'rb').read()
    try:
        validate_container(container)
    except ValueError as error:
        sys.exit(str(error))
    blob = prefix + container + b'\0' * (-len(container) % 16)
    packer = zlib.compressobj(9, zlib.DEFLATED, 31, 8)
    with tempfile.TemporaryDirectory(prefix='reseal-') as tmp:
        kernel, rd = os.path.join(tmp, 'kernel'), os.path.join(tmp, 'ramdisk')
        open(kernel, 'wb').write(packer.compress(blob) + packer.flush() + dtb)
        open(rd, 'wb').write(ramdisk)
        subprocess.check_call([sys.executable, MKBOOTIMG, '--kernel', kernel, '--ramdisk', rd,
                               '--header_version', '1', '--os_version', '15.0.0',
                               '--os_patch_level', '2026-02', '-o', args.output])
    data = open(args.output, 'rb').read()
    print(f'{args.output}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}; '
          f'shim+FD {len(prefix)} bytes, container {len(container)} bytes, DTB {len(dtb)} bytes')


if __name__ == '__main__':
    main()

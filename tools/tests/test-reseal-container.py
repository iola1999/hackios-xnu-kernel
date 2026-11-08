#!/usr/bin/env python3
"""Check resealing inputs before the boot-image writer can create an output."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/reseal-loader-img.py'
spec = importlib.util.spec_from_file_location('reseal', TOOL)
reseal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reseal)


def payload(entries):
    body = b''
    for name, data in entries:
        body += struct.pack('<16sII', name, len(data), 0) + data + bytes(-len(data) % 16)
    body += bytes(24)
    return b'XNUP' + struct.pack('<I', len(body)) + body


class ContainerTests(unittest.TestCase):
    def test_direct_standalone(self):
        reseal.validate_container(payload([(b'loader.efi', b'efi'), (b'kernel', b'kernel'),
                                           (b'ramdisk', b'helper'), (b'rdimage', b'boot')]))

    def test_debug_loader(self):
        reseal.validate_container(payload([(b'loader.efi', b'efi'), (b'usb', b'1\0')]))

    def test_nested_compression(self):
        with self.assertRaisesRegex(ValueError, 'payload.lz4 is unsupported'):
            reseal.validate_container(payload([(b'loader.efi', b'efi'), (b'payload.lz4', b'lz4')]))

    def test_incomplete_or_extra_bytes(self):
        original = payload([(b'kernel', b'kernel')])
        for changed in (b'', b'XNUP', original[:-1], original + b'\0'):
            with self.subTest(length=len(changed)), self.assertRaises(ValueError):
                reseal.validate_container(changed)

    def test_bad_entry(self):
        original = bytearray(payload([(b'kernel', b'kernel')]))
        for offset, value in ((8, b'x' * 16), (24, struct.pack('<I', 4096)),
                              (28, b'\1'), (38, b'\1')):
            changed = bytearray(original)
            changed[offset:offset + len(value)] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                reseal.validate_container(changed)

    def test_bad_terminator(self):
        original = payload([(b'kernel', b'kernel')])
        for body in (original[8:-24], original[8:-1] + b'\1', original[8:] + bytes(24)):
            with self.subTest(size=len(body)), self.assertRaises(ValueError):
                reseal.validate_container(b'XNUP' + struct.pack('<I', len(body)) + body)

    def test_cli_rejects_before_output(self):
        with tempfile.TemporaryDirectory(prefix='reseal-test-') as directory:
            root = Path(directory)
            reference = root / 'reference.img'
            compressed = zlib.compressobj(wbits=31)
            kernel = compressed.compress(b'shim' + payload([(b'loader.efi', b'efi')])) + compressed.flush()
            header = bytearray(4096)
            header[:8] = b'ANDROID!'
            struct.pack_into('<I', header, 8, len(kernel))
            struct.pack_into('<I', header, 0x24, 4096)
            reference.write_bytes(header + kernel)
            nested = root / 'nested.bin'
            nested.write_bytes(payload([(b'payload.lz4', b'lz4')]))
            output = root / 'output.img'
            output.write_bytes(b'preserve-existing-output')
            result = subprocess.run([sys.executable, str(TOOL), str(reference), str(nested),
                                     '-o', str(output)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('payload.lz4 is unsupported', result.stderr)
            self.assertEqual(output.read_bytes(), b'preserve-existing-output')


if __name__ == '__main__':
    unittest.main()

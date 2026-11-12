#!/usr/bin/env python3
"""Check V5 container validation and preserve multi-stream/register semantics.

Optional arguments are local vendor modules for an additional full decode.
Vendor files are not test fixtures in the repository.
"""

from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from camsensor_v5 import Module, inspect


def fixture():
    pool, numeric = bytearray(752), []

    def add(raw):
        sid = len(numeric) + 1
        numeric.append((sid, len(pool), len(raw), sid))
        pool.extend(raw)
        return sid

    empty = add(b'')
    name = add(b'test-sensor\0')
    channel = add(struct.pack('<2I', 0, 2))
    data = add(struct.pack('<2I', 0x24, 0x35))
    delay = add(struct.pack('<I', 1234))
    setting = struct.pack('<14I', 0, empty, 0, empty, 0, empty, 0x3000, 2, data, 2, 1, 1, 1, delay)
    settings = add(setting)
    stream = struct.pack('<21I', 2, channel, 0x2b, 0, 0, 640, 480, 10, 1,
                         0, empty, 0, empty, 0, empty, 0, empty, 0, empty, 0, empty)
    streams = add(stream + stream)
    mode = bytearray(408)
    struct.pack_into('<I', mode, 24, 4)
    struct.pack_into('<2I', mode, 28, 2, streams)
    struct.pack_into('<2I', mode, 44, 800, 525)
    struct.pack_into('<d', mode, 72, 30.0)
    struct.pack_into('<I', mode, 96, 4)
    struct.pack_into('<I', mode, 124, 35)
    struct.pack_into('<2I', mode, 140, 1, settings)
    modes = add(mode)
    version = add(struct.pack('<I', 0))
    info = add(struct.pack('<4I', 1, version, 1, modes))
    struct.pack_into('<2I', pool, 12, 12, name)
    struct.pack_into('<7I', pool, 64, 0x20, 2, 2, 0x300a, 0x5650, 0xffff, 3)
    for offset in (108, 116, 564):
        struct.pack_into('<2I', pool, offset, 0, empty)
    struct.pack_into('<2I', pool, 324, 1, info)
    for offset in (492, 500, 508, 516):
        struct.pack_into('<2I', pool, offset, 1, settings)
    named = struct.pack('<I32s6I', 1, b'sensorDriverData', 1, 0, 0, 0, 752, 1)
    table = b''.join(struct.pack('<4I', *n) for n in numeric)
    sections = (named, pool, b'', table)
    output = bytearray(216)
    signature = b'QTI Chromatix Header\0'
    output[:len(signature)] = signature
    label = b'Parameter Parser V5.2.1 (2408261003)\0'
    output[0x28:0x28 + len(label)] = label
    struct.pack_into('<HHI', output, 0x20, 5, 0, 0)
    struct.pack_into('<I', output, 0xa4, 4)
    for kind, payload in enumerate(sections):
        struct.pack_into('<3I', output, 0xa8 + kind * 12, kind, len(output), len(payload))
        output.extend(payload)
    struct.pack_into('<I', output, 0x1c, len(output))
    output.extend(struct.pack('<I', zlib.crc32(output)))
    return bytes(output)


def altered(source, offset, value):
    raw = bytearray(source)
    struct.pack_into('<I', raw, offset, value)
    struct.pack_into('<I', raw, len(raw) - 4, zlib.crc32(raw[:-4]))
    return bytes(raw)


class Tests(unittest.TestCase):
    def test_preserves_streams_values_and_delay(self):
        report = inspect(fixture())
        mode = report['resolutionInfo'][0]['resolutionData'][0]
        self.assertEqual(len(mode['streams']), 2)
        self.assertEqual(mode['streams'][0]['virtualChannel'], [0, 2])
        self.assertEqual(mode['colorFilterArrangement'], 4)
        setting = mode['resSettings'][0]
        self.assertEqual(setting['registerData'], [0x24, 0x35])
        self.assertEqual((setting['operation'], setting['delayUs']), (1, 1234))

    def test_rejects_damaged_container(self):
        source = fixture()
        damaged = bytearray(source)
        damaged[-1] ^= 1
        for raw in (source[:-1], damaged, altered(source, 0xb8, 0), altered(source, 0x20, 6),
                    altered(source, 0x28, 0)):
            with self.subTest():
                with self.assertRaises(ValueError):
                    inspect(raw)

    def test_rejects_alias_and_array_corruption(self):
        source = fixture()
        module = Module(source)
        numeric_at = struct.unpack_from('<I', source, 0xd0)[0]
        for offset, value in ((numeric_at + 4, module.pool_size + 1), (numeric_at + 12, 0),
                              (module.pool + 324, 2), (module.pool + 328, 0xffffffff)):
            with self.subTest(offset=offset):
                with self.assertRaises(ValueError):
                    inspect(altered(source, offset, value))

    def test_rejects_data_width_and_operation(self):
        source = fixture()
        module = Module(source)
        setting_at = module.pool + module.numeric[6][0]
        data_at = module.pool + module.numeric[4][0]
        for offset, value in ((setting_at + 36, 0), (setting_at + 40, 5), (setting_at + 44, 5),
                              (setting_at + 24, 0x10000), (data_at, 256), (data_at + 4, 256)):
            with self.subTest(offset=offset):
                with self.assertRaises(ValueError):
                    inspect(altered(source, offset, value))


if __name__ == '__main__':
    for path in sys.argv[1:]:
        report = inspect(Path(path).read_bytes())
        modes = [m for r in report['resolutionInfo'] for m in r['resolutionData']]
        print(f"decoded {report['sensorName']}: {len(modes)} modes; source {report['source_sha256']}")
    unittest.main(argv=[sys.argv[0]])

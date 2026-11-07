#!/usr/bin/env python3
"""Check production DSC decoding against Apple v3/v5 C bitfield layouts."""
from pathlib import Path
import importlib.util
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'dsc/cache.py'
spec = importlib.util.spec_from_file_location('dsc_cache', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.path.insert(0, str(SOURCE.parent))
import vtables
BASE = 0x180000000

# Field order/widths: apple-oss-distributions/dyld include/mach-o/
# dyld_cache_format.h (dyld_cache_slide_pointer3), fixup-chains.h
# (dyld_chained_ptr_arm64e_shared_cache_rebase and *_auth_rebase).
LAYOUT = r'''
#include <stdint.h>
#include <stdio.h>
union v3 {
 uint64_t raw;
 struct { uint64_t pointerValue:51, next:11, unused:2; } plain;
 struct { uint64_t offset:32, diversity:16, addrDiv:1, key:2, next:11, unused:1, auth:1; } auth;
};
union v5 {
 uint64_t raw;
 struct { uint64_t runtimeOffset:34, high8:8, unused:10, next:11, auth:1; } regular;
 struct { uint64_t runtimeOffset:34, diversity:16, addrDiv:1, keyIsData:1, next:11, auth:1; } auth;
};
_Static_assert(sizeof(union v3)==8 && sizeof(union v5)==8,"pointer size");
static void emit(unsigned version,uint64_t raw,uint64_t target) {
 printf("%u %llx %llx\n",version,(unsigned long long)raw,(unsigned long long)target);
}
int main(void) {
 union v3 p3={0}; union v5 p5={0};
 p3.auth.offset=0xf1234567; p3.auth.diversity=0xffff; p3.auth.addrDiv=1;
 p3.auth.key=3; p3.auth.next=0x7ff; p3.auth.auth=1;
 emit(3,p3.raw,0x271234567ULL);
 p3.raw=0; p3.plain.pointerValue=0x29ffca62cULL; p3.plain.next=0x7ff;
 emit(3,p3.raw,0x29ffca62cULL);
 p3.plain.pointerValue=(0xabULL<<43)|0x29ffca62cULL;
 emit(3,p3.raw,0xab0000029ffca62cULL);
 p5.auth.runtimeOffset=0x11ffa11e0ULL; p5.auth.diversity=0xffff;
 p5.auth.addrDiv=1; p5.auth.keyIsData=1; p5.auth.next=0x7ff; p5.auth.auth=1;
 emit(5,p5.raw,0x29ffa11e0ULL);
 p5.auth.runtimeOffset=0x3ffffffffULL;
 emit(5,p5.raw,0x57fffffffULL);
 p5.raw=0; p5.regular.runtimeOffset=0x100000010ULL; p5.regular.next=0x7ff;
 emit(5,p5.raw,0x280000010ULL);
 p5.regular.high8=0xab;
 emit(5,p5.raw,0xab00000280000010ULL);
}
'''


def write_cache(directory, layouts, arch='arm64e'):
    data = bytearray(0x5000)
    data[:16] = ('dyld_v1  ' + arch).encode().ljust(16, b'\0')
    struct.pack_into('<II', data, 0x10, 0x400, len(layouts))
    struct.pack_into('<QQ', data, 0x88, 0, 0)
    struct.pack_into('<QQ', data, 0xe0, BASE, 0x400000000)
    struct.pack_into('<II', data, 0x138, 0x500, len(layouts))
    for i, (version, base) in enumerate(layouts):
        address, fileoff, slide = BASE + i * 0x1000, 0x1000 + i * 0x1000, 0x700 + i * 0x40
        struct.pack_into('<QQQII', data, 0x400 + i * 32, address, 0x1000, fileoff, 3, 3)
        struct.pack_into('<QQQQQQII', data, 0x500 + i * 56,
                         address, 0x1000, fileoff, slide, 26, 0, 3, 3)
        struct.pack_into('<III4xQH', data, slide, version, 0x1000, 1, base, 0xffff)
    path = directory / ('dyld_shared_cache_' + arch)
    path.write_bytes(data)
    return path


class CacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix='dsc-layout-') as name:
            directory = Path(name)
            source, binary = directory / 'layout.c', directory / 'layout'
            source.write_text(LAYOUT)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True)
            cls.words = [(int(v), int(raw, 16), int(target, 16))
                         for v, raw, target in (line.split() for line in output.splitlines())]

    def make_cache(self, layouts, arch='arm64e', implementation=module.Cache):
        temporary = tempfile.TemporaryDirectory(prefix='dsc-cache-')
        self.addCleanup(temporary.cleanup)
        write_cache(Path(temporary.name), layouts, arch)
        cache = implementation(temporary.name)
        self.addCleanup(cache.close)
        return cache

    def test_compiled_layouts(self):
        for version in (3, 5):
            cache = self.make_cache([(version, BASE)])
            self.assertIsNone(cache.decode(0))
            for layout, raw, target in self.words:
                if layout == version:
                    self.assertEqual(cache.decode(raw), target)
                    self.assertEqual(cache.decode(raw, BASE), target)

    def test_measured_h264sw_pointer(self):
        cache = self.make_cache([(5, BASE)])
        self.assertEqual(cache.decode(0x801000011ffa11e0), 0x29ffa11e0)

    def test_mapping_specific_versions_and_bases(self):
        cache = self.make_cache([(3, BASE), (5, BASE + 0x100000000)])
        raw = 0x8000000000000010
        with self.assertRaises(module.CacheError):
            cache.decode(raw)
        self.assertEqual(cache.decode(raw, BASE), BASE + 0x10)
        self.assertEqual(cache.decode(raw, BASE + 0x1000), BASE + 0x100000010)
        with self.assertRaises(module.CacheError):
            cache.decode(raw, BASE + 0x3000)

    def test_unsupported_version(self):
        cache = self.make_cache([(7, BASE)])
        with self.assertRaisesRegex(module.CacheError, 'version 7'):
            cache.decode(0x8000000000000010)

    def test_vtable_entrypoints_with_mixed_mappings(self):
        cache = self.make_cache([(3, BASE), (5, BASE)])
        image = SimpleNamespace(path='fixture')
        cache.image_of = lambda _address: image
        sections = []
        for i, offset in enumerate((0x100, 0x100000100)):
            address = BASE + i * 0x1000
            path, fileoff, _prot = cache.where(address + 16)
            with open(path, 'r+b') as stream:
                stream.seek(fileoff)
                stream.write(struct.pack('<QQQ', *(0x8000000000000000 | (offset + n * 8)
                                                  for n in range(3))))
            section = SimpleNamespace(addr=address, size=40, name='__const')
            sections.append((SimpleNamespace(name='__DATA_CONST'), section))
            with patch.object(vtables, '_text_range', return_value=(BASE, BASE + 0x200000000)):
                decoded = vtables.slots(cache, address, count=3)
            self.assertEqual([item[3] for item in decoded], [BASE + offset + n * 8 for n in range(3)])
        with patch.object(vtables, '_text_range', return_value=(BASE, BASE + 0x200000000)), \
                patch.object(vtables, 'find_sections', return_value=sections):
            hits = vtables.scan(cache, image)
        self.assertEqual([(hit[0], hit[1]) for hit in hits], [(BASE, 3), (BASE + 0x1000, 3)])

    def test_arm64_unchanged(self):
        cache = self.make_cache([(2, BASE)], arch='arm64')
        self.assertEqual(cache.decode(0xabc0000190000010), 0x190000010)

    def test_truncated_slide_header(self):
        with tempfile.TemporaryDirectory(prefix='dsc-bad-') as name:
            path = write_cache(Path(name), [(5, BASE)])
            data = bytearray(path.read_bytes())
            struct.pack_into('<Q', data, 0x500 + 32, 16)
            path.write_bytes(data)
            with self.assertRaisesRegex(module.CacheError, 'invalid slide info'):
                module.Cache(name)

    def test_v5_truncation_mutation(self):
        namespace = {'__name__': 'dsc_mutant'}
        source = SOURCE.read_text()
        self.assertIn('(1 << 34)', source)
        exec(source.replace('(1 << 34)', '(1 << 32)'), namespace)
        cache = self.make_cache([(5, BASE)], implementation=namespace['Cache'])
        self.assertNotEqual(cache.decode(0x801000011ffa11e0), 0x29ffa11e0)


if __name__ == '__main__':
    unittest.main()

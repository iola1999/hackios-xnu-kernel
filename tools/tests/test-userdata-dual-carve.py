#!/usr/bin/env python3
"""tools/userdata-dual-carve.py: a carve of the phone's own LUN 0 GPT backup (annibale-backup/, skipped when it
is not there), and synthetic GPTs for darwin_nvram in a new carve and with --split-nvram."""

import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/userdata-dual-carve.py'
BACKUP_GPT = ROOT.parent / 'annibale-backup/20261022-2355/gpt'
spec = importlib.util.spec_from_file_location('carve', SCRIPT)
carve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(carve)

BS = 4096
GIB = 1 << 30
DISK = 256 * GIB // BS
COUNT, ES = 128, 128
TABLE_BLOCKS = COUNT * ES // BS
FIRST_USABLE, LAST_USABLE = 2 + TABLE_BLOCKS, DISK - 2 - TABLE_BLOCKS


def gpt(parts, backup_attrs=None):
    """gpt-head.bin and gpt-tail.bin for parts: (slot, name, first, last); backup_attrs: {slot: attributes}
    that only the backup table has."""
    def table_of(attrs):
        table = bytearray(COUNT * ES)
        for slot, name, first, last in parts:
            at = slot * ES
            table[at:at + 16] = carve.APFS_CONTAINER.bytes_le
            table[at + 16:at + 32] = uuid.UUID(int=slot + 1).bytes_le
            struct.pack_into('<QQQ', table, at + 32, first, last, attrs.get(slot, 0))
            table[at + 56:at + 56 + 2 * len(name)] = name.encode('utf-16le')
        return table

    def header(my, alternate, entries_lba, table):
        h = bytearray(BS)
        struct.pack_into('<8sIII', h, 0, b'EFI PART', 0x10000, 92, 0)
        struct.pack_into('<QQQQ', h, 24, my, alternate, FIRST_USABLE, LAST_USABLE)
        h[56:72] = uuid.UUID(int=7).bytes_le
        struct.pack_into('<QIII', h, 72, entries_lba, COUNT, ES, zlib.crc32(table))
        struct.pack_into('<I', h, 16, zlib.crc32(h[:92]))
        return h

    primary, backup = table_of({}), table_of(backup_attrs or {})
    head = bytearray(8 * BS)
    head[BS:2 * BS] = header(1, DISK - 1, 2, primary)
    head[2 * BS:2 * BS + len(primary)] = primary
    tail = bytearray(8 * BS)
    at = (DISK - 1 - TABLE_BLOCKS - (DISK - 8)) * BS
    tail[at:at + len(backup)] = backup
    tail[7 * BS:] = header(DISK - 1, 1, DISK - 1 - TABLE_BLOCKS, backup)
    return bytes(head), bytes(tail)


def apfs_block0(blocks):
    b = bytearray(BS)
    b[32:36] = b'NXSB'
    struct.pack_into('<IQ', b, 36, BS, blocks)
    return bytes(b)


VAR_FIRST = 120 * GIB // BS
INSTALLED = [(0, 'boot', FIRST_USABLE, 1000), (33, 'userdata', 1001, 100 * GIB // BS - 1),
             (34, 'darwin', 100 * GIB // BS, VAR_FIRST - 1), (35, 'darwin_var', VAR_FIRST, LAST_USABLE)]


class Carve(unittest.TestCase):
    def run_tool(self, parts, *args, block0=None, backup_attrs=None):
        d = tempfile.mkdtemp(prefix='carve-')
        head, tail = gpt(parts, backup_attrs)
        Path(d, 'gpt-head.bin').write_bytes(head)
        Path(d, 'gpt-tail.bin').write_bytes(tail)
        if block0 is not None:
            Path(d, 'darwin_var-block0.bin').write_bytes(block0)
        r = subprocess.run([sys.executable, str(SCRIPT), d, *args], capture_output=True, text=True)
        return r, d

    def entries(self, d, same=True):
        head = Path(d, 'gpt-head.new').read_bytes()
        tail = Path(d, 'gpt-tail.new').read_bytes()
        ok, _, _, entries = carve.check('test', head, tail, DISK, same=same)
        self.assertTrue(ok)
        return {e['name']: e for e in entries if e['type'].int != 0}

    def test_split_keeps_backup_attributes(self):
        var_blocks = LAST_USABLE - VAR_FIRST + 1
        r, d = self.run_tool(INSTALLED, '--split-nvram', block0=apfs_block0(var_blocks - carve.NVRAM_BLOCKS),
                             backup_attrs={0: 1 << 50})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.entries(d, same=False)
        tail = Path(d, 'gpt-tail.new').read_bytes()
        bh = carve.header_of(tail[7 * BS:])
        _, be, _ = carve.entries_of(tail, bh, DISK - 8)
        self.assertEqual(be[0]['attrs'], 1 << 50)
        self.assertEqual(be[36]['name'], 'darwin_nvram')
        head = Path(d, 'gpt-head.new').read_bytes()
        _, pe, _ = carve.entries_of(head, carve.header_of(head[BS:2 * BS]), 0)
        self.assertEqual(pe[0]['attrs'], 0)
        self.assertEqual(pe[36], be[36])

    def test_split(self):
        var_blocks = LAST_USABLE - VAR_FIRST + 1
        r, d = self.run_tool(INSTALLED, '--split-nvram', block0=apfs_block0(var_blocks - carve.NVRAM_BLOCKS))
        self.assertEqual(r.returncode, 0, r.stderr)
        e = self.entries(d)
        self.assertEqual(e['darwin_var']['last'], LAST_USABLE - carve.NVRAM_BLOCKS)
        self.assertEqual((e['darwin_nvram']['first'], e['darwin_nvram']['last']),
                         (LAST_USABLE - carve.NVRAM_BLOCKS + 1, LAST_USABLE))
        self.assertEqual(e['darwin_nvram']['type'], carve.NVRAM_TYPE)
        self.assertEqual({k: (v['first'], v['last']) for k, v in e.items() if k not in ('darwin_var', 'darwin_nvram')},
                         {name: (first, last) for _, name, first, last in INSTALLED[:3]})
        self.assertIn('sda37', r.stdout)

    def test_split_refuses_container_in_tail(self):
        var_blocks = LAST_USABLE - VAR_FIRST + 1
        r, d = self.run_tool(INSTALLED, '--split-nvram', block0=apfs_block0(var_blocks - carve.NVRAM_BLOCKS + 1))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('reaches into the last 32 MiB', r.stderr)
        self.assertFalse(os.path.exists(os.path.join(d, 'gpt-head.new')))

    def test_split_refuses_annfs(self):
        block0 = bytearray(BS)
        block0[:8] = b'ANNFS001'
        r, _ = self.run_tool(INSTALLED, '--split-nvram', block0=bytes(block0))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('not an APFS container', r.stderr)

    def test_split_refuses_twice(self):
        parts = INSTALLED[:3] + [(35, 'darwin_var', VAR_FIRST, LAST_USABLE - carve.NVRAM_BLOCKS),
                                 (36, 'darwin_nvram', LAST_USABLE - carve.NVRAM_BLOCKS + 1, LAST_USABLE)]
        r, _ = self.run_tool(parts, '--split-nvram', block0=apfs_block0(100))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('already exists', r.stderr)

    def test_new_carve(self):
        parts = [(0, 'boot', FIRST_USABLE, 1000), (33, 'userdata', 1001, LAST_USABLE)]
        r, d = self.run_tool(parts, '--root-gib', '16', '--var-gib', '104')
        self.assertEqual(r.returncode, 0, r.stderr)
        e = self.entries(d)
        self.assertEqual(e['darwin_nvram']['last'], LAST_USABLE)
        self.assertEqual(e['darwin_nvram']['last'] - e['darwin_nvram']['first'] + 1, carve.NVRAM_BLOCKS)
        self.assertEqual(e['darwin_var']['last'] + 1, e['darwin_nvram']['first'])
        self.assertLess(e['userdata']['last'], e['darwin']['first'])
        self.assertLess(e['darwin']['last'], e['darwin_var']['first'])
        self.assertEqual(e['darwin']['last'] - e['darwin']['first'] + 1, 16 * GIB // BS)

    @unittest.skipUnless((BACKUP_GPT / 'sda-head.bin').exists(), 'no LUN 0 GPT backup in annibale-backup/')
    def test_real_backup(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copy(BACKUP_GPT / 'sda-head.bin', td)
            shutil.copy(BACKUP_GPT / 'sda-tail.bin', td)
            r = subprocess.run([sys.executable, str(SCRIPT), td, '--root-gib', '8', '--var-gib', '112'],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(os.path.getsize(os.path.join(td, 'gpt-head.new')), 32768)
            self.assertEqual(os.path.getsize(os.path.join(td, 'gpt-tail.new')), 32768)
            self.assertIn('userdata:   LBA 3518464..32584698 (110.879 GiB', r.stdout)
            self.assertIn('darwin (root): LBA 32584704..34681855 (8.000 GiB', r.stdout)
            self.assertIn('darwin_var (var):  LBA 34681856..64033786 (111.969 GiB', r.stdout)
            self.assertIn('darwin_nvram: LBA 64033787..64041978 (32 MiB', r.stdout)
            self.assertIn('Total iOS Carved: 120.000 GiB', r.stdout)


if __name__ == '__main__':
    unittest.main()

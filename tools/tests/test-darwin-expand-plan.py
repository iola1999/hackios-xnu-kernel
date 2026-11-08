#!/usr/bin/env python3
"""Check offline GPT expansion plans using synthetic primary and backup tables."""

import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/darwin-expand-plan.py"
spec = importlib.util.spec_from_file_location("darwin_expand_plan", SCRIPT)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
GIB = 1 << 30
DISK_BYTES = 96 * GIB
SS = 4096
COUNT = 64
ENTRY_SIZE = 128
USERDATA, DARWIN, VAR = 32, 33, 34


def fixture(sector=SS, entry_size=ENTRY_SIZE, root_bytes=8 * GIB):
    blocks = DISK_BYTES // sector
    table_blocks = (COUNT * entry_size + sector - 1) // sector
    backup_lba = blocks - 1 - table_blocks
    align = max(1, SS // sector)
    first_usable = ((2 + table_blocks + align - 1) // align) * align
    last_usable = (backup_lba // align) * align - 1
    root_first = 24 * GIB // sector
    root_last = root_first + root_bytes // sector - 1
    table = bytearray(COUNT * entry_size)
    for index, name, first, last, attrs in (
        (0, "boot", first_usable, GIB // sector - 1, 0),
        (USERDATA, "userdata", GIB // sector, root_first - 1, 0x10),
        (DARWIN, "darwin", root_first, root_last, 0x20),
        (VAR, "darwin_var", root_last + 1, last_usable, 0x40),
    ):
        at = index * entry_size
        table[at:at + 16] = tool.APFS_TYPE.bytes_le
        table[at + 16:at + 32] = uuid.UUID(int=index + 100).bytes_le
        struct.pack_into("<QQQ", table, at + 32, first, last, attrs)
        table[at + 56:at + 56 + len(name) * 2] = name.encode("utf-16le")
        table[at + 128:at + entry_size] = b"x" * (entry_size - 128)

    def header(current, alternate, entries_lba):
        block = bytearray(sector)
        tool.HEADER.pack_into(block, 0, b"EFI PART", 0x10000, 92, 0, 0,
                              current, alternate, first_usable, last_usable,
                              uuid.UUID(int=1).bytes_le, entries_lba, COUNT,
                              entry_size, zlib.crc32(table))
        struct.pack_into("<I", block, 16, zlib.crc32(block[:92]))
        return block

    backup_blocks = max(8, table_blocks + 2)
    head = bytearray(backup_blocks * sector)
    tail = bytearray(backup_blocks * sector)
    head[sector:2 * sector] = header(1, blocks - 1, 2)
    head[2 * sector:2 * sector + len(table)] = table
    tail[-sector:] = header(blocks - 1, 1, backup_lba)
    at = (backup_lba - (blocks - backup_blocks)) * sector
    tail[at:at + len(table)] = table
    return head, tail


def header_at(blob, primary, sector=SS):
    return sector if primary else len(blob) - sector


def seal_header(blob, primary, sector=SS):
    at = header_at(blob, primary, sector)
    size = struct.unpack_from("<I", blob, at + 12)[0]
    struct.pack_into("<I", blob, at + 16, 0)
    struct.pack_into("<I", blob, at + 16, zlib.crc32(blob[at:at + size]))


def patch_header(blob, primary, offset, fmt, value, sector=SS):
    struct.pack_into(fmt, blob, header_at(blob, primary, sector) + offset, value)
    seal_header(blob, primary, sector)


def patch_entry(head, tail, index, offset, value, fmt=None):
    for blob, primary in ((head, True), (tail, False)):
        at = header_at(blob, primary)
        entries_lba, count, size = struct.unpack_from("<QII", blob, at + 72)
        base = 0 if primary else DISK_BYTES - len(blob)
        table_at = entries_lba * SS - base
        position = table_at + index * size + offset
        if fmt is None:
            blob[position:position + len(value)] = value
        else:
            struct.pack_into(fmt, blob, position, value)
        struct.pack_into("<I", blob, at + 88, zlib.crc32(blob[table_at:table_at + count * size]))
        seal_header(blob, primary)


def metadata(block=None):
    annfs = tool.annfs
    root = [2, 0, 2, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0] + [0] * 8
    payload = annfs.INODE.pack(*root)
    if block is not None:
        file = [3, annfs.PAGE_SIZE, 2, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0] + [0] * 8
        payload += annfs.INODE.pack(*file) + struct.pack("<II", block, 0)
    result = bytearray(annfs.METADATA_BYTES)
    annfs.SUPER.pack_into(result, 0, b"ANNFS001", 3, annfs.PAGE_SIZE, 1,
                         len(payload), 4, 1 if block is None else 2, zlib.crc32(payload), 0)
    struct.pack_into("<I", result, 48, zlib.crc32(result[:48]))
    result[4096:4096 + len(payload)] = payload
    return result


class ExpansionTest(unittest.TestCase):
    def test_preserve_guid_attrs_userdata_and_other_entry_bytes(self):
        head, tail = fixture(entry_size=256)
        before = bytes(head), bytes(tail)
        result = tool.plan(head, tail, DISK_BYTES, SS)
        self.assertEqual(result["changed_entries"], [DARWIN + 1, VAR + 1])
        self.assertEqual(result["darwin"]["after_bytes"], 16 * GIB)
        self.assertEqual(result["darwin_var"]["before_bytes"] - result["darwin_var"]["after_bytes"], 8 * GIB)
        for partition in ("darwin", "darwin_var"):
            for field in ("guid", "type_guid", "attrs", "name", "entry"):
                self.assertEqual(result[partition]["before"][field], result[partition]["after"][field])
        self.assertEqual(result["userdata_unchanged"]["last_lba"], 24 * GIB // SS - 1)
        expected = bytearray(tool.parse_gpt(head, DISK_BYTES, SS, True)["table"])
        struct.pack_into("<Q", expected, DARWIN * 256 + 40, 40 * GIB // SS - 1)
        struct.pack_into("<Q", expected, VAR * 256 + 32, 40 * GIB // SS)
        self.assertEqual(result["proposed_entry_array_sha256"], tool.digest(expected))
        self.assertTrue(result["all_other_entry_bytes_unchanged"])
        self.assertFalse(result["data_fit_checked"])
        self.assertIsNone(result["var_transfer_ranges"])
        self.assertEqual((head, tail), before)

    def test_sector_size_512_and_actual_backup_lengths(self):
        head, tail = fixture(sector=512)
        result = tool.plan(head, tail, DISK_BYTES, 512)
        self.assertEqual(result["darwin"]["after"]["last_lba"], 40 * GIB // 512 - 1)
        self.assertEqual(result["rollback"]["gpt_ranges"][1]["offset"], DISK_BYTES - len(tail))

    def test_already_sized_partition_has_no_changed_entries(self):
        head, tail = fixture(root_bytes=16 * GIB)
        self.assertEqual(tool.plan(head, tail, DISK_BYTES, SS)["changed_entries"], [])

    def test_header_and_entry_crcs(self):
        for which, at, expected in ((0, SS + 56, "header CRC"), (1, -SS + 56, "header CRC"),
                                    (0, 2 * SS + 48, "entry array CRC")):
            pair = list(fixture())
            pair[which][at] ^= 1
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(*pair, DISK_BYTES, SS)

    def test_actual_disk_size_is_required(self):
        head, tail = fixture()
        with self.assertRaisesRegex(ValueError, "actual disk size"):
            tool.plan(head, tail, DISK_BYTES + SS, SS)

    def test_disk_guid_and_usable_range_match(self):
        for offset, fmt, value in ((56, "<Q", 99), (40, "<Q", 9), (48, "<Q", DISK_BYTES // SS - 5)):
            head, tail = fixture()
            patch_header(tail, False, offset, fmt, value)
            with self.assertRaisesRegex(ValueError, "headers disagree"):
                tool.plan(head, tail, DISK_BYTES, SS)

    def test_entry_arrays_must_match(self):
        head, tail = fixture()
        at = len(tail) - SS
        table_lba = struct.unpack_from("<Q", tail, at + 72)[0]
        table_at = table_lba * SS - (DISK_BYTES - len(tail))
        tail[table_at + 48] ^= 1
        struct.pack_into("<I", tail, at + 88, zlib.crc32(tail[table_at:table_at + COUNT * ENTRY_SIZE]))
        seal_header(tail, False)
        with self.assertRaisesRegex(ValueError, "disagree"):
            tool.plan(head, tail, DISK_BYTES, SS)

    def test_partition_bounds_and_overlap(self):
        for index, offset, value, expected in (
            (0, 32, 0, "usable bounds"),
            (VAR, 40, DISK_BYTES // SS, "usable bounds"),
            (USERDATA, 40, 24 * GIB // SS, "overlap"),
            (VAR, 32, 32 * GIB // SS + 1, "not adjacent"),
        ):
            head, tail = fixture()
            patch_entry(head, tail, index, offset, value, "<Q")
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(head, tail, DISK_BYTES, SS)

    def test_invalid_partition_identities(self):
        for index, offset, value, expected in (
            (DARWIN, 16, bytes(16), "GUID"),
            (DARWIN, 16, uuid.UUID(int=VAR + 100).bytes_le, "GUID"),
            (DARWIN, 0, uuid.UUID(int=999).bytes_le, "not APFS"),
            (DARWIN, 56, "darwin_var".encode("utf-16le"), "exactly one darwin"),
            (USERDATA, 56, bytes(72), "exactly one userdata"),
            (DARWIN, 56, b"\0\xd8", "UTF-16"),
        ):
            head, tail = fixture()
            patch_entry(head, tail, index, offset, value)
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(head, tail, DISK_BYTES, SS)

    def test_header_shape_and_table_placement(self):
        for offset, fmt, value, expected in (
            (12, "<I", 91, "header size"),
            (8, "<I", 0x20000, "revision"),
            (20, "<I", 1, "reserved"),
            (80, "<I", 0, "entry layout"),
            (84, "<I", 129, "entry layout"),
            (72, "<Q", 1, "overlaps"),
            (80, "<I", 1 << 30, "overlaps"),
        ):
            head, tail = fixture()
            patch_header(head, True, offset, fmt, value)
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(head, tail, DISK_BYTES, SS)

    def test_backup_shape_and_truncated_tables(self):
        head, tail = fixture()
        for a, b, expected in ((head[:-1], tail, "aligned"),
                               (head[:2 * SS], tail, "truncated"),
                               (head, tail[-2 * SS:], "truncated")):
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(a, b, DISK_BYTES, SS)

    def test_target_capacity_and_alignment(self):
        head, tail = fixture()
        for size, expected in ((4 * GIB, "shrink"), (96 * GIB, "exhausts"), (16 * GIB + 1, "aligned")):
            with self.assertRaisesRegex(ValueError, expected):
                tool.plan(head, tail, DISK_BYTES, SS, size)
        with self.assertRaisesRegex(ValueError, "target darwin_var capacity"):
            tool.plan(head, tail, DISK_BYTES, SS, 72 * GIB - 8 * SS)
        with self.assertRaisesRegex(ValueError, "sector size"):
            tool.plan(head, tail, DISK_BYTES, 1024)

    def test_var_ranges_and_rollback_coordinates(self):
        head, tail = fixture()
        result = tool.plan(head, tail, DISK_BYTES, SS, var_metadata=metadata(131072))
        self.assertTrue(result["data_fit_checked"])
        self.assertTrue(result["data_fits"])
        self.assertEqual(result["var_metadata_status"], "checkpoint")
        self.assertEqual(result["var_transfer_ranges"], [
            {"source_offset": 32 * GIB, "target_offset": 40 * GIB, "length": 512 << 20},
            {"source_offset": 34 * GIB + (512 << 20), "target_offset": 42 * GIB + (512 << 20),
             "length": tool.annfs.PAGE_SIZE},
        ])
        rollback = result["rollback"]
        self.assertEqual(rollback["old_system_range"], {"offset": 24 * GIB, "length": 8 * GIB})
        self.assertEqual(rollback["old_var_ranges"][0], {"offset": 32 * GIB, "length": 512 << 20})
        self.assertTrue(rollback["backup_required_before_destination_writes"])
        self.assertFalse(rollback["var_copy_overlaps_old_referenced_ranges"])
        self.assertFalse(rollback["gpt_only_rollback_after_system_write"])

    def test_var_copy_detects_overlap_with_old_referenced_ranges(self):
        head, tail = fixture()
        block = (8 * GIB - tool.annfs.DATA_OFFSET) // tool.annfs.PAGE_SIZE
        result = tool.plan(head, tail, DISK_BYTES, SS, var_metadata=metadata(block))
        self.assertTrue(result["rollback"]["var_copy_overlaps_old_referenced_ranges"])

    def test_var_tail_reference_blocks_expansion(self):
        head, tail = fixture()
        before = tool.plan(head, tail, DISK_BYTES, SS)["darwin_var"]["before_bytes"]
        block = (before - tool.annfs.DATA_OFFSET) // tool.annfs.PAGE_SIZE - 1
        result = tool.plan(head, tail, DISK_BYTES, SS, var_metadata=metadata(block))
        self.assertFalse(result["data_fits"])
        self.assertIsNone(result["var_transfer_ranges"])
        self.assertIsNotNone(result["rollback"]["old_var_ranges"])
        self.assertGreater(result["var_minimum_bytes"], result["darwin_var"]["after_bytes"])

    def test_invalid_var_metadata_prevents_data_fit(self):
        head, tail = fixture()
        blob = metadata()
        blob[4096] ^= 1
        result = tool.plan(head, tail, DISK_BYTES, SS, var_metadata=blob)
        self.assertFalse(result["data_fits"])
        self.assertIsNone(result["var_transfer_ranges"])
        self.assertIsNone(result["rollback"]["old_var_ranges"])
        self.assertEqual(result["var_metadata_status"], "invalid")

    def test_cli_is_read_only_and_rejects_oversized_input(self):
        with tempfile.TemporaryDirectory(prefix="darwin-expand-test-") as directory:
            paths = [Path(directory) / name for name in ("head.bin", "tail.bin")]
            for path, blob in zip(paths, fixture()):
                path.write_bytes(blob)
            before = [path.stat().st_mtime_ns for path in paths]
            command = [sys.executable, str(SCRIPT), *map(str, paths),
                       "--disk-bytes", str(DISK_BYTES), "--sector-bytes", str(SS)]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)["darwin"]["after_bytes"], 16 * GIB)
            self.assertEqual([path.stat().st_mtime_ns for path in paths], before)
            with paths[0].open("r+b") as file:
                file.truncate(tool.MAX_BACKUP_BYTES + 1)
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("input limit", json.loads(result.stdout)["error"])

    def test_candidate_bytes_only_change_two_lbas_and_crcs(self):
        head, tail = fixture(entry_size=256)
        with tempfile.TemporaryDirectory(prefix="darwin-gpt-output-") as directory:
            output = Path(directory) / "new"
            result = tool.plan(head, tail, DISK_BYTES, SS, output_dir=output)
            self.assertEqual(set(path.name for path in output.iterdir()),
                             {"gpt-head.new.bin", "gpt-tail.new.bin", "plan.json"})
            self.assertEqual(json.loads((output / "plan.json").read_text()), result)
            candidates = [(output / result["candidate_files"][name]).read_bytes() for name in ("head", "tail")]
            for old, new, primary in ((head, candidates[0], True), (tail, candidates[1], False)):
                header = header_at(old, primary)
                entries_lba = struct.unpack_from("<Q", old, header + 72)[0]
                base = 0 if primary else DISK_BYTES - len(old)
                table_at = entries_lba * SS - base
                allowed = set()
                for at, length in ((header + 16, 4), (header + 88, 4),
                                   (table_at + DARWIN * 256 + 40, 8), (table_at + VAR * 256 + 32, 8)):
                    allowed.update(range(at, at + length))
                self.assertEqual(len(old), len(new))
                changed = {at for at, (a, b) in enumerate(zip(old, new)) if a != b}
                self.assertTrue(changed)
                self.assertLessEqual(changed, allowed)
            checked = tool.plan(*candidates, DISK_BYTES, SS)
            self.assertEqual(checked["changed_entries"], [])
            self.assertEqual(checked["darwin"]["after_bytes"], 16 * GIB)
            self.assertEqual(checked["darwin_var"]["before"], result["darwin_var"]["after"])
            self.assertEqual(result["candidate_sha256"], dict(zip(("head", "tail"), map(tool.digest, candidates))))

    def test_candidate_output_refuses_existing_paths(self):
        head, tail = fixture()
        with tempfile.TemporaryDirectory(prefix="darwin-gpt-refusal-") as directory:
            root = Path(directory)
            regular = root / "file"
            regular.write_bytes(b"keep")
            existing = root / "directory"
            existing.mkdir()
            link = root / "symlink"
            link.symlink_to(existing)
            fifo = root / "fifo"
            os.mkfifo(fifo)
            for output in (regular, existing, link, fifo):
                with self.assertRaises(FileExistsError):
                    tool.plan(head, tail, DISK_BYTES, SS, output_dir=output)
            self.assertEqual(regular.read_bytes(), b"keep")
            self.assertEqual(list(existing.iterdir()), [])
            with self.assertRaisesRegex(ValueError, "regular host file"):
                tool.read_backup(fifo)
            with self.assertRaisesRegex(ValueError, "regular host file"):
                tool.read_backup("/dev/null")

    def test_invalid_var_fit_creates_no_candidate_directory(self):
        head, tail = fixture()
        source_bytes = tool.plan(head, tail, DISK_BYTES, SS)["darwin_var"]["before_bytes"]
        block = (source_bytes - tool.annfs.DATA_OFFSET) // tool.annfs.PAGE_SIZE - 1
        with tempfile.TemporaryDirectory(prefix="darwin-gpt-var-") as directory:
            output = Path(directory) / "new"
            with self.assertRaisesRegex(ValueError, "does not fit"):
                tool.plan(head, tail, DISK_BYTES, SS, var_metadata=metadata(block), output_dir=output)
            self.assertFalse(output.exists())

    def test_cli_candidate_output_and_existing_directory_refusal(self):
        with tempfile.TemporaryDirectory(prefix="darwin-gpt-cli-") as directory:
            root = Path(directory)
            paths = [root / name for name in ("head.bin", "tail.bin")]
            for path, blob in zip(paths, fixture()):
                path.write_bytes(blob)
            before = [path.read_bytes() for path in paths]
            output = root / "candidates"
            command = [sys.executable, str(SCRIPT), *map(str, paths), "--disk-bytes", str(DISK_BYTES),
                       "--sector-bytes", str(SS), "--output-dir", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            report = json.loads(result.stdout)
            self.assertEqual(report, json.loads((output / "plan.json").read_text()))
            again = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(again.returncode, 2)
            self.assertEqual([path.read_bytes() for path in paths], before)


if __name__ == "__main__":
    unittest.main()

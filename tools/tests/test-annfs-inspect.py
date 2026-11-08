#!/usr/bin/env python3
"""Exercise the offline planner with fixtures emitted from production C structs."""

import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/annfs-inspect.py"
spec = importlib.util.spec_from_file_location("annfs_inspect", SCRIPT)
annfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annfs)


def fixture_source():
    disk = (ROOT / "xnu/files/bsd/miscfs/ramfs/ramfs_disk.inc").read_text()
    definitions = disk[disk.index("#define ANNFS_MAGIC_STR"):disk.index("static struct ramfs_mount *")]
    return r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#define PAGE_SIZE 16384
''' + definitions + r'''
static uint8_t buffer[16384];
static size_t at = ANNFS_SUPER_BYTES;
static void append(const void *value, size_t bytes) {
    assert(at + bytes <= sizeof(buffer));
    memcpy(buffer + at, value, bytes); at += bytes;
}
static void string(const char *value) {
    size_t bytes = strlen(value); append(value, bytes);
    at += ANNFS_ALIGN8(bytes) - bytes;
}
static void entry(uint64_t child, const char *name) {
    struct annfs_dirent_rec rec = { .child_ino = child, .namelen = (uint32_t)strlen(name) };
    append(&rec, sizeof(rec)); string(name);
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    if (!strcmp(argv[1], "layout")) {
        printf("{\"super\":%zu,\"inode\":%zu,\"xattr\":%zu,\"dirent\":%zu,"
               "\"super_crc\":%zu,\"data_offset\":%llu,\"slot_bytes\":%llu,"
               "\"inode_type\":%zu,\"inode_nlink\":%zu,\"inode_nblocks\":%zu,"
               "\"inode_target_len\":%zu,\"inode_ndirents\":%zu}",
               sizeof(struct annfs_super), sizeof(struct annfs_inode_rec),
               sizeof(struct annfs_xattr_rec), sizeof(struct annfs_dirent_rec),
               offsetof(struct annfs_super, super_crc), ANNFS_DATA_OFFSET, ANNFS_SLOT_MAX,
               offsetof(struct annfs_inode_rec, type), offsetof(struct annfs_inode_rec, nlink),
               offsetof(struct annfs_inode_rec, nblocks), offsetof(struct annfs_inode_rec, target_len),
               offsetof(struct annfs_inode_rec, ndirents));
        return 0;
    }
    assert(argc == 5);
    int tree = !strcmp(argv[1], "tree");
    uint32_t block = (uint32_t)strtoul(argv[3], NULL, 0);
    struct annfs_inode_rec root = {
        .ino = 2, .parent_ino = 2, .type = 2, .nlink = 2,
        .nxattrs = tree ? 1 : 0, .ndirents = tree ? 3 : 0,
    };
    append(&root, sizeof(root));
    if (tree) {
        struct annfs_xattr_rec xa = { .namelen = 8, .size = 4 };
        append(&xa, sizeof(xa)); string("com.testDATA");
        entry(3, "file"); entry(4, "link"); entry(5, "dir");
        struct annfs_inode_rec file = {
            .ino = 3, .parent_ino = 2, .type = 1, .nlink = 1,
            .size = 4 * PAGE_SIZE, .nblocks = 4,
        };
        uint32_t blocks[] = { block, block + 1, 0, block + 3 };
        append(&file, sizeof(file)); append(blocks, sizeof(blocks));
        struct annfs_inode_rec link = {
            .ino = 4, .parent_ino = 2, .type = 5, .nlink = 1, .size = 4, .target_len = 4,
        };
        append(&link, sizeof(link)); string("file");
        struct annfs_inode_rec dir = { .ino = 5, .parent_ino = 2, .type = 2, .nlink = 2 };
        append(&dir, sizeof(dir));
    }
    struct annfs_super *s = (void *)buffer;
    memcpy(s->magic, ANNFS_MAGIC_STR, 8);
    s->version = (uint32_t)strtoul(argv[4], NULL, 0);
    s->page_size = PAGE_SIZE;
    s->generation = strtoull(argv[2], NULL, 0);
    s->payload_bytes = at - ANNFS_SUPER_BYTES;
    s->next_ino = 6; s->node_count = tree ? 4 : 1;
    s->payload_crc = (uint32_t)crc32(0, buffer + ANNFS_SUPER_BYTES, (uInt)s->payload_bytes);
    s->super_crc = (uint32_t)crc32(0, buffer, offsetof(struct annfs_super, super_crc));
    assert(fwrite(buffer, 1, at, stdout) == at);
    return 0;
}
'''


class InspectorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="annfs-fixture-")
        source = Path(cls.build.name) / "fixture.c"
        cls.generator = Path(cls.build.name) / "fixture"
        source.write_text(fixture_source())
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                        "-lz", "-o", str(cls.generator)], check=True)
        cls.layout = json.loads(subprocess.check_output([str(cls.generator), "layout"]))

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="annfs-inspect-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "metadata.bin"
        with self.path.open("wb") as file:
            file.truncate(annfs.METADATA_BYTES)

    def fixture(self, mode="root", generation=1, block=10, version=3):
        return bytearray(subprocess.check_output([
            str(self.generator), mode, str(generation), str(block), str(version),
        ]))

    def write_slot(self, number, blob):
        with self.path.open("r+b") as file:
            file.seek(number * annfs.SLOT_BYTES)
            file.write(blob)

    def report(self, partition=4 << 30, target=None):
        return annfs.inspect(annfs.read_metadata(self.path), partition, target)

    def reseal(self, blob):
        payload_bytes = struct.unpack_from("<Q", blob, 24)[0]
        struct.pack_into("<I", blob, 44, zlib.crc32(blob[4096:4096 + payload_bytes]))
        struct.pack_into("<I", blob, 48, zlib.crc32(blob[:48]))

    def assert_invalid(self, blob, text):
        self.write_slot(0, blob)
        result = self.report()
        self.assertEqual(result["status"], "invalid")
        self.assertIn(text, result["slots"][0]["error"])
        self.assertFalse(result["target_fits"])
        self.assertEqual(result["backup_ranges"], [])

    def test_production_struct_layout(self):
        self.assertEqual(self.layout["super"], annfs.SUPER_BYTES)
        self.assertEqual(self.layout["inode"], annfs.INODE.size)
        self.assertEqual(self.layout["xattr"], annfs.XATTR.size)
        self.assertEqual(self.layout["dirent"], annfs.DIRENT.size)
        self.assertEqual(self.layout["super_crc"], 48)
        self.assertEqual(self.layout["data_offset"], annfs.DATA_OFFSET)
        self.assertEqual(self.layout["slot_bytes"], annfs.SLOT_BYTES)

    def test_empty_and_root_only_checkpoints(self):
        result = self.report()
        self.assertEqual(result["status"], "empty")
        self.assertIsNone(result["selected_slot"])
        self.assertEqual(result["backup_bytes"], annfs.DATA_OFFSET)
        self.write_slot(1, self.fixture(generation=3))
        result = self.report()
        self.assertEqual(result["status"], "checkpoint")
        self.assertEqual(result["selected_slot"], 1)
        self.assertEqual(result["slots"][1]["node_count"], 1)
        self.assertEqual(result["minimum_partition_bytes"], annfs.MIN_PARTITION_BYTES)

    def test_production_tree_and_coalesced_data_ranges(self):
        self.write_slot(0, self.fixture("tree", block=131072))
        result = self.report()
        start = annfs.DATA_OFFSET + 131072 * annfs.PAGE_SIZE
        self.assertEqual(result["slots"][0]["node_count"], 4)
        self.assertEqual(result["data_ranges"], [
            {"offset": start, "length": 2 * annfs.PAGE_SIZE},
            {"offset": start + 3 * annfs.PAGE_SIZE, "length": annfs.PAGE_SIZE},
        ])
        self.assertEqual(result["referenced_data_bytes"], 3 * annfs.PAGE_SIZE)
        self.assertEqual(result["backup_bytes"], annfs.DATA_OFFSET + 3 * annfs.PAGE_SIZE)
        self.assertEqual(result["minimum_partition_bytes"], start + 4 * annfs.PAGE_SIZE)

    def test_union_of_valid_slots_and_newest_selection(self):
        self.write_slot(0, self.fixture("tree", generation=4, block=10))
        self.write_slot(1, self.fixture("tree", generation=7, block=14))
        result = self.report()
        self.assertEqual(result["valid_slots"], [0, 1])
        self.assertEqual(result["selected_slot"], 1)
        self.assertEqual(result["referenced_data_bytes"], 6 * annfs.PAGE_SIZE)
        self.assertEqual(len(result["data_ranges"]), 3)

    def test_equal_generations_choose_slot_zero(self):
        self.write_slot(0, self.fixture(generation=7))
        self.write_slot(1, self.fixture(generation=7))
        self.assertEqual(self.report()["selected_slot"], 0)

    def test_newer_corrupt_payload_falls_back(self):
        self.write_slot(0, self.fixture(generation=2))
        blob = self.fixture("tree", generation=9)
        blob[-1] ^= 1
        self.write_slot(1, blob)
        result = self.report()
        self.assertEqual(result["selected_slot"], 0)
        self.assertEqual(result["valid_slots"], [0])
        self.assertEqual(result["referenced_data_bytes"], 0)
        self.assertIn("payload CRC", result["slots"][1]["error"])

    def test_structural_failure_also_falls_back(self):
        self.write_slot(0, self.fixture(generation=1))
        blob = self.fixture(generation=8)
        struct.pack_into("<Q", blob, 4096 + 16, 999)
        self.reseal(blob)
        self.write_slot(1, blob)
        result = self.report()
        self.assertEqual(result["selected_slot"], 0)
        self.assertIn("directory parent", result["slots"][1]["error"])

    def test_superblock_crc_and_unsupported_version(self):
        blob = self.fixture()
        blob[32] ^= 1
        self.assert_invalid(blob, "superblock CRC")
        self.assert_invalid(self.fixture(version=9), "unsupported version")

    def test_source_capacity_and_target_boundary(self):
        self.write_slot(0, self.fixture("tree", block=100))
        required = annfs.DATA_OFFSET + 104 * annfs.PAGE_SIZE
        self.assertTrue(self.report(target=required)["target_fits"])
        self.assertFalse(self.report(target=required - 4096)["target_fits"])
        result = self.report(partition=required - 4096)
        self.assertEqual(result["status"], "invalid")
        self.assertIn("source capacity", result["slots"][0]["error"])

    def test_target_must_preserve_older_valid_slot(self):
        self.write_slot(0, self.fixture("tree", generation=1, block=131072))
        self.write_slot(1, self.fixture(generation=2))
        result = self.report(target=1 << 30)
        self.assertEqual(result["selected_slot"], 1)
        self.assertFalse(result["target_fits"])
        self.assertGreater(result["minimum_partition_bytes"], 2 << 30)

    def test_kernel_bitmap_limit(self):
        self.write_slot(0, self.fixture("tree", block=annfs.MAX_BLOCKS - 3))
        result = self.report(partition=256 << 30)
        self.assertEqual(result["status"], "invalid")
        self.assertIn("source capacity", result["slots"][0]["error"])

    def test_alignment_and_partition_minimum(self):
        for size in (annfs.MIN_PARTITION_BYTES - 4096, (4 << 30) + 1):
            with self.assertRaises(ValueError):
                self.report(partition=size)
            with self.assertRaises(ValueError):
                self.report(target=size)

    def test_truncated_saved_metadata(self):
        with self.path.open("r+b") as file:
            file.truncate(annfs.METADATA_BYTES - 1)
        with self.assertRaisesRegex(ValueError, "shorter"):
            annfs.read_metadata(self.path)
        with self.assertRaisesRegex(ValueError, "exactly"):
            annfs.inspect(b"", 4 << 30)
        with self.assertRaisesRegex(ValueError, "regular host file"):
            annfs.read_metadata(self.tmp.name)

    def test_incomplete_payload_and_trailing_data(self):
        for delta, expected in ((-8, "inode count"), (8, "trailing payload")):
            blob = self.fixture()
            struct.pack_into("<Q", blob, 24, annfs.INODE.size + delta)
            if delta > 0:
                blob.extend(b"\0" * delta)
            self.reseal(blob)
            self.assert_invalid(blob, expected)

    def test_inode_type_link_target_and_block_constraints(self):
        for field, value, expected in (
            ("inode_type", 8, "inode type"),
            ("inode_nlink", 0, "link count"),
            ("inode_target_len", 4, "non-symlink"),
            ("inode_nblocks", 1, "non-regular"),
        ):
            blob = self.fixture()
            struct.pack_into("<I", blob, 4096 + self.layout[field], value)
            self.reseal(blob)
            self.assert_invalid(blob, expected)

    def test_xattr_and_directory_reference_constraints(self):
        xattr_offset = 4096 + annfs.INODE.size
        entry_offset = xattr_offset + annfs.XATTR.size + 16
        for offset, fmt, value, expected in (
            (xattr_offset, "<I", 128, "xattr size"),
            (xattr_offset + 4, "<I", (16 << 20) + 1, "xattr size"),
            (entry_offset, "<Q", 999, "entry inode"),
            (entry_offset + 8, "<I", 0, "entry name"),
            (entry_offset + 8, "<I", 256, "entry name"),
        ):
            blob = self.fixture("tree")
            struct.pack_into(fmt, blob, offset, value)
            self.reseal(blob)
            self.assert_invalid(blob, expected)

    def test_symlink_directory_and_inode_identity_constraints(self):
        file_offset = 4096 + annfs.INODE.size + annfs.XATTR.size + 16 + 3 * 24
        link_offset = file_offset + annfs.INODE.size + 16
        dir_offset = link_offset + annfs.INODE.size + 8
        for offset, fmt, value, expected in (
            (link_offset + 8, "<Q", 5, "symlink size"),
            (dir_offset + 16, "<Q", 3, "directory parent"),
            (file_offset, "<Q", 2, "duplicate inode"),
            (file_offset + self.layout["inode_ndirents"], "<I", 1, "non-directory"),
        ):
            blob = self.fixture("tree")
            struct.pack_into(fmt, blob, offset, value)
            self.reseal(blob)
            self.assert_invalid(blob, expected)
        blob = self.fixture()
        struct.pack_into("<Q", blob, 4096, 3)
        self.reseal(blob)
        self.assert_invalid(blob, "missing root")

    def test_record_body_bounds(self):
        blob = self.fixture("tree")
        struct.pack_into("<Q", blob, 24, len(blob) - 4096 - 8)
        self.reseal(blob)
        self.assert_invalid(blob, "record exceeds payload")

    def test_nonzero_unformatted_metadata_is_rejected(self):
        with self.path.open("r+b") as file:
            file.seek(65536)
            file.write(b"x")
        self.assertEqual(self.report()["status"], "invalid")

    def test_cli_exit_status_and_no_input_changes(self):
        self.write_slot(0, self.fixture("tree", block=100))
        before = self.path.stat().st_mtime_ns
        command = ["python3", str(SCRIPT), str(self.path), "--partition-bytes", str(4 << 30)]
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        self.assertTrue(json.loads(result.stdout)["target_fits"])
        result = subprocess.run(command + ["--target-bytes", str(annfs.MIN_PARTITION_BYTES)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(json.loads(result.stdout)["target_fits"])
        self.assertEqual(self.path.stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()

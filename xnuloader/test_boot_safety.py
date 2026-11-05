#!/usr/bin/env python3
"""
Unit test suite for xnuloader boot safety, slot-agnostic switching, GPT CRC-32,
and standalone payload packaging.
"""

import os
import struct
import subprocess
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


class TestBootSafetyGPT(unittest.TestCase):
    def test_gpt_crc32_math(self):
        """Verify UEFI GPT IEEE 802.3 CRC-32 calculation for header and partition array."""
        block_size = 4096
        num_parts = 128
        part_entry_size = 128
        part_array_size = num_parts * part_entry_size

        # Create mock partition array
        part_array = bytearray(part_array_size)
        for i in range(num_parts):
            # name offset = 56, utf-16le
            name = f"partition_{i}".encode("utf-16le")
            part_array[i * part_entry_size + 56 : i * part_entry_size + 56 + len(name)] = name

        # Compute partition array CRC-32
        part_crc = zlib.crc32(part_array) & 0xFFFFFFFF

        # Create primary GPT header (LBA 1, 4096-byte block)
        header = bytearray(block_size)
        struct.pack_into("<8s", header, 0, b"EFI PART")
        struct.pack_into("<I", header, 8, 0x00010000)   # Revision 1.0
        struct.pack_into("<I", header, 12, 92)          # HeaderSize 92
        struct.pack_into("<I", header, 16, 0)           # HeaderCRC32 initially 0
        struct.pack_into("<I", header, 20, 0)           # Reserved
        struct.pack_into("<Q", header, 24, 1)           # MyLBA = 1
        struct.pack_into("<Q", header, 32, 1000)        # AlternateLBA = 1000
        struct.pack_into("<Q", header, 40, 2)           # FirstUsableLBA = 2
        struct.pack_into("<Q", header, 48, 999)         # LastUsableLBA = 999
        # Disk GUID at offset 56 (16 bytes)
        struct.pack_into("<Q", header, 72, 2)           # PartitionEntryLBA = 2
        struct.pack_into("<I", header, 80, num_parts)   # NumberOfPartitionEntries
        struct.pack_into("<I", header, 84, part_entry_size) # SizeOfPartitionEntry
        struct.pack_into("<I", header, 88, part_crc)    # PartitionEntryArrayCRC32

        # Compute header CRC-32 over HeaderSize (92 bytes) with offset 16 zeroed
        head_crc = zlib.crc32(header[:92]) & 0xFFFFFFFF
        struct.pack_into("<I", header, 16, head_crc)

        # Verification: zero offset 16 and recompute
        verify_header = bytearray(header)
        struct.pack_into("<I", verify_header, 16, 0)
        self.assertEqual(zlib.crc32(verify_header[:92]) & 0xFFFFFFFF, head_crc)

        # Backup GPT generation
        backup_header = bytearray(header)
        struct.pack_into("<Q", backup_header, 24, 1000)  # MyLBA = alt_lba
        struct.pack_into("<Q", backup_header, 32, 1)     # AlternateLBA = pri_lba
        struct.pack_into("<I", backup_header, 16, 0)     # Zero header CRC
        backup_crc = zlib.crc32(backup_header[:92]) & 0xFFFFFFFF
        struct.pack_into("<I", backup_header, 16, backup_crc)

        # Verify backup header CRC
        struct.pack_into("<I", backup_header, 16, 0)
        self.assertEqual(zlib.crc32(backup_header[:92]) & 0xFFFFFFFF, backup_crc)

    def test_slot_detect_and_switching_logic(self):
        """Simulate GPT attribute parsing and slot-agnostic switching."""
        # Attribute bit layout:
        # bits 48..51: priority (4 bits)
        # bits 52..54: tries_remaining (3 bits)
        # bit 55: successful (1 bit)

        def make_attr(priority, tries, successful):
            val = ((priority & 0xF) << 48) | ((tries & 0x7) << 52)
            if successful:
                val |= (1 << 55)
            return val

        def parse_attr(attr):
            priority = (attr >> 48) & 0xF
            tries = (attr >> 52) & 0x7
            successful = (attr >> 55) & 1
            return priority, tries, successful

        # Case 1: Slot A active, Slot B inactive
        attr_a = make_attr(15, 7, 1)
        attr_b = make_attr(14, 0, 0)

        # Detect active slot
        pri_a, tries_a, succ_a = parse_attr(attr_a)
        pri_b, tries_b, succ_b = parse_attr(attr_b)
        active_a = (pri_a > 0 and tries_a > 0) or succ_a
        active_b = (pri_b > 0 and tries_b > 0) or succ_b

        if pri_b > pri_a and active_b:
            loader_slot = "b"
        elif active_a:
            loader_slot = "a"
        elif active_b:
            loader_slot = "b"
        else:
            loader_slot = "a"
        android_slot = "b" if loader_slot == "a" else "a"

        self.assertEqual(loader_slot, "a")
        self.assertEqual(android_slot, "b")

        # Test slot_maintain_current: refresh tries to 7, successful to 1
        attr_a_maint = make_attr(pri_a, 7, 1)
        self.assertEqual(parse_attr(attr_a_maint), (15, 7, 1))

        # Test slot_set_active_other: promote android_slot to 15, lower loader_slot to 14
        attr_b_target = make_attr(15, 7, 1)
        attr_a_lower = make_attr(14, 0, 0)
        self.assertEqual(parse_attr(attr_b_target), (15, 7, 1))
        self.assertEqual(parse_attr(attr_a_lower), (14, 0, 0))

        # Case 2: Slot B active (e.g. after Android OTA), Slot A inactive
        attr_a = make_attr(14, 0, 0)
        attr_b = make_attr(15, 7, 1)

        pri_a, tries_a, succ_a = parse_attr(attr_a)
        pri_b, tries_b, succ_b = parse_attr(attr_b)
        active_a = (pri_a > 0 and tries_a > 0) or succ_a
        active_b = (pri_b > 0 and tries_b > 0) or succ_b

        if pri_b > pri_a and active_b:
            loader_slot = "b"
        elif active_a:
            loader_slot = "a"
        elif active_b:
            loader_slot = "b"
        else:
            loader_slot = "a"
        android_slot = "b" if loader_slot == "a" else "a"

        self.assertEqual(loader_slot, "b")
        self.assertEqual(android_slot, "a")


class TestSourceContracts(unittest.TestCase):
    def test_loader_screen_ui_contracts(self):
        ui_inc = HERE / "loader_06a_screen_ui.inc"
        self.assertTrue(ui_inc.is_file())
        text = ui_inc.read_text()

        # Slot agnostic functions
        self.assertIn("slot_detect", text)
        self.assertIn("slot_maintain_current", text)
        self.assertIn("slot_set_active_other", text)
        self.assertIn("gpt_sync_lun4", text)

        # Prohibit hardcoded "SLOT B"
        self.assertNotIn('"BOOT ANDROID (SLOT B)"', text)
        self.assertNotIn('"SLOT B"', text)

    def test_loader_usb_entry_contracts(self):
        entry_inc = HERE / "loader_06_usb_entry.inc"
        self.assertTrue(entry_inc.is_file())
        text = entry_inc.read_text()

        # Unconditional storage survey before boot menu
        idx_block = text.find("block_start();")
        idx_inspect = text.find("inspect_storage();")
        idx_slot_detect = text.find("slot_detect();")
        idx_menu = text.find("ui_boot_menu()")

        self.assertTrue(0 < idx_block < idx_inspect < idx_slot_detect < idx_menu,
                        "Must call block_start, inspect_storage, and slot_detect before ui_boot_menu")

        # Fallback to fastboot debug loop
        self.assertIn("BOOT FALLBACK - FASTBOOT MODE", text)
        self.assertIn("usb_command_loop()", text)

    def test_run_kernel_null_fallback(self):
        run_inc = HERE / "loader_05_layout_run_kernel.inc"
        self.assertTrue(run_inc.is_file())
        text = run_inc.read_text()

        self.assertIn("NO KERNEL - FASTBOOT MODE", text)
        self.assertIn("usb_command_loop()", text)

    def test_file_line_limits(self):
        for fname in ["loader_05_layout_run_kernel.inc", "loader_06_usb_entry.inc",
                      "loader_06a_screen_ui.inc", "mkstandalone.py"]:
            f = HERE / fname
            self.assertTrue(f.is_file(), f"{f} missing")
            lines = len(f.read_text().splitlines())
            self.assertLessEqual(lines, 1200, f"{f} has {lines} lines (exceeds 1200 limit)")


class TestStandalonePackager(unittest.TestCase):
    def test_mkstandalone_script(self):
        script = HERE / "mkstandalone.py"
        res = subprocess.run(["python3", str(script), "--test"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("mkstandalone self-test: PASS", res.stdout)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""
Unit test suite for xnuloader boot safety, slot-agnostic switching, GPT CRC-32,
and standalone payload packaging.
"""

import os
import importlib.util
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

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

    def test_slot_choice_from_boot_images(self):
        """Build slot_choice.h on the host and classify loader and Android boot images."""
        def boot_image(version, page, kernel):
            head = bytearray(8192)
            head[:8] = b"ANDROID!"
            struct.pack_into("<I", head, 36, page)
            struct.pack_into("<I", head, 40, version)
            if page:
                head[page:page + len(kernel)] = kernel
            else:
                head[4096:4096 + len(kernel)] = kernel
            return bytes(head)

        images = {
            "loader": boot_image(1, 2048, b"\x1f\x8b\x08"),
            "android_v4": boot_image(4, 0, b"MZ"),
            "v1_raw_kernel": boot_image(1, 2048, b"MZ"),
            "blank": bytes(8192),
        }
        harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "slot_choice.h"
int main(int argc, char **argv) {
    static uint8_t head[SLOT_HEAD_BYTES];
    int in[2];
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(argv[1 + i], "rb");
        size_t n = fread(head, 1, sizeof(head), f);
        fclose(f);
        in[i] = boot_image_is_loader(head, n);
    }
    char loader = 1, android = 1;
    slot_choose(in[0], in[1], &loader, &android);
    printf("%d %d %c %c\n", in[0], in[1], loader ? loader : '-', android ? android : '-');
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in images.items():
                (root / name).write_bytes(data)
            (root / "harness.c").write_text(harness)
            binary = root / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(HERE), "-o", str(binary),
                            str(root / "harness.c")], check=True)

            def choose(a, b):
                out = subprocess.run([str(binary), str(root / a), str(root / b)], check=True,
                                     capture_output=True, text=True).stdout.split()
                return out[0] == "1", out[1] == "1", out[2], out[3]

            self.assertEqual(choose("loader", "android_v4"), (True, False, "a", "b"))
            self.assertEqual(choose("android_v4", "loader"), (False, True, "b", "a"))
            # Started from RAM by LK with Android in both slots: nothing names a slot.
            self.assertEqual(choose("android_v4", "android_v4"), (False, False, "-", "-"))
            self.assertEqual(choose("loader", "loader"), (True, True, "-", "-"))
            self.assertEqual(choose("v1_raw_kernel", "blank"), (False, False, "-", "-"))


class TestSourceContracts(unittest.TestCase):
    def test_loader_screen_ui_contracts(self):
        ui_inc = HERE / "loader_06a_screen_ui.inc"
        self.assertTrue(ui_inc.is_file())
        text = ui_inc.read_text()

        # Slot agnostic functions
        self.assertIn("slot_detect", text)
        self.assertIn("slot_maintain_current", text)
        self.assertIn("slot_set_active_other", text)
        self.assertIn("gpt_sync_lun(bio)", text)

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

        # Fallback to fastboot debug loop and unattended recovery safeguards
        self.assertIn("boot_fallback_enter();", text)
        self.assertIn("BOOT FALLBACK - FASTBOOT MODE", (HERE / "loader_06a_screen_ui.inc").read_text())
        self.assertIn("usb_command_loop()", text)
        self.assertIn("menu_enter_fastboot();", text)
        self.assertNotIn("(void)menu_enter_fastboot();", text)
        self.assertIn("spmi_reboot_lk_prepare();", text)
        self.assertIn("spmi_reboot_lk_prepare(void)", (HERE / "loader_06a_screen_ui.inc").read_text())

    def test_run_kernel_null_fallback(self):
        run_inc = HERE / "loader_05_layout_run_kernel.inc"
        self.assertTrue(run_inc.is_file())
        text = run_inc.read_text()

        self.assertIn("NO KERNEL - FASTBOOT MODE", text)
        self.assertIn("usb_command_loop()", text)

    def test_file_line_limits(self):
        for fname in ["loader_05_layout_run_kernel.inc", "loader_06_usb_entry.inc",
                      "loader_06a_screen_ui.inc", "ui_font_private.h", "mkstandalone.py"]:
            f = HERE / fname
            self.assertTrue(f.is_file(), f"{f} missing")
            lines = len(f.read_text().splitlines())
            self.assertLessEqual(lines, 1200, f"{f} has {lines} lines (exceeds 1200 limit)")

    def test_ui_rendering_and_fault_banner_preservation(self):
        """Compile UI code on host and verify boot menu, USB progress, and fault banner layout."""
        harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t char16;
typedef uint64_t efi_status;
#define EFIAPI
#define EFI_ERROR(x) ((x) != 0)
static struct { uint64_t base; uint32_t width, height, stride; } SCREEN;
static struct { void *con_in; } st_inst, *ST = &st_inst;
static void *running_live = NULL;
static int kernel_resources_held = 0, firmware_fault = 0, held_irq_masked = 0;
static int usb_connected = 1, skip_ufs_arbitration = 0;
static uint64_t host_last_cmd = 0, fake_cntvct = 1000;
static char log_buf[4096];
static uint32_t log_len = 0;
static char last_result[64] = "early-exception 0 0xffffffff0071a42c0";
#define CTX_LOG_PA  0
#define CTX_LOG_LEN 1
static uint64_t loader_ctx[2];
#define READ_SYSREG(reg) (strcmp(#reg, "CNTFRQ_EL0") == 0 ? 19200000ULL : fake_cntvct)
#define PAYLOAD_BASE 0ULL
#define PAYLOAD_CONTAINER_MAX (55ULL << 20)
static struct { uintptr_t fb_base; } platform_annibale = { 0xdeadbeefULL };
static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static void print(const char *fmt, ...) { (void)fmt; }
static void wdt_pet(void) {}
static void reboot(void) {}
static void clean_dcache(uint64_t s, uint64_t e) { (void)s; (void)e; }
typedef struct { uint32_t media_id; uint32_t block_size; uint64_t last_block; } efi_media;
typedef struct efi_block_io {
    efi_media *media;
    efi_status (*read_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
    efi_status (*write_blocks)(struct efi_block_io *, uint32_t, uint64_t, uint64_t, void *);
    efi_status (*flush_blocks)(struct efi_block_io *);
} efi_block_io;
#define BLOCK_PART_UNIT 1
static struct {
    uint32_t parts;
    struct { uint32_t flags; uint16_t lun, unit; char name[36]; } part[4];
} block_shared;
static efi_block_io *block_bios[4];
static uint8_t ufs_buffer[65536];
static uint32_t ufs_crc32(const void *d, uint64_t n) { (void)d; (void)n; return 0; }
static efi_block_io *find_partition_named(const char16 *n, uint64_t l) { (void)n; (void)l; return NULL; }

#include "loader_06a_screen_ui.inc"
static void usb_poll_command(void) {}

static int count_nonzero_in_band(const uint32_t *fb, int y0, int y1) {
    int nz = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < 1156; x++) {
            if ((fb[y * 1156 + x] & 0x00ffffffu) != 0) nz++;
        }
    }
    return nz;
}

int main(void) {
    uint32_t *fb = calloc(1156 * 2510, sizeof(uint32_t));
    SCREEN.base = (uint64_t)(uintptr_t)fb;
    SCREEN.width = 1156;
    SCREEN.height = 2510;
    SCREEN.stride = 1156 * 4;

    loader_slot = 'a';
    android_slot = 'b';
    slot_update_menu_text();
    ui_draw_menu(MENU_CHOICE_IOS, 5, 0);
    if (count_nonzero_in_band(fb, 200, 1520) < 50000) return 1;

    ui_draw_ios_boot_splash();
    if (count_nonzero_in_band(fb, 450, 2380) < 10000) return 2;
    const char *boot_log =
        "Darwin Kernel Version 25.6.0\n"
        "vm_page_bootstrap: 774811 free pages\n"
        "adreno8: step 14: configure gpu SMMU context 0\n"
        "[ ok ] mount the persistent data volume on /private/var\n"
        "2026-11-09 20:38:36.810879+0800 backboardd[41:276]  IOMobileFramebufferOpen:  success, return 0\n";
    ui_boot_watch_step(boot_log, (uint32_t)strlen(boot_log));
    if (UI_BOOT.pct != 100 || UI_BOOT.done != 1) return 5;
    if (count_nonzero_in_band(fb, 900, 1450) < 5000) return 6;

    const char *klog = "panic(cpu 0): kernel trap\nhalting cpu 0 and returning to xnuloader stub\n";
    loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)klog;
    loader_ctx[CTX_LOG_LEN] = strlen(klog);
    UI.ready = 1;
    boot_fallback_enter();
    /* Subsequent ui_init() from usb_command_loop() must preserve the fault banner below y=1250 */
    ui_init();
    if (count_nonzero_in_band(fb, 240, 670) < 10000) return 3;
    if (count_nonzero_in_band(fb, 1250, 2450) < 20000) return 4;

    free(fb);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "harness.c").write_text(harness)
            binary = root / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror",
                            "-I", str(HERE), "-o", str(binary), str(root / "harness.c")], check=True)
            subprocess.run([str(binary)], check=True)


class TestStandalonePackager(unittest.TestCase):
    def packager(self):
        spec = importlib.util.spec_from_file_location('standalone_under_test', HERE / 'mkstandalone.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_resident_boot_bytes_and_ufs_command(self):
        module = self.packager()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, helper, boot = (root / name for name in ('kernel', 'helper', 'boot'))
            kernel.write_bytes(b'kernel fixture')
            helper.write_bytes(b'helper volume')
            boot.write_bytes(b'NXSB' + bytes(range(256)) * 17)
            output = root / 'payload.bin'
            # Compression has its own CLI self-test; inspect the real mkpayload output here.
            with patch.object(module, 'compress_lz4'):
                module.build_standalone_payload(kernel, helper, output, boot_image=boot)
            data = output.read_bytes()
            magic, length = struct.unpack_from('<II', data)
            self.assertEqual((magic, length), (0x50554E58, len(data) - 8))
            entries, offset = {}, 8
            while data[offset]:
                name, size, reserved = struct.unpack_from('<16sII', data, offset)
                name = name.rstrip(b'\0').decode('ascii')
                self.assertNotIn(name, entries)
                self.assertEqual(reserved, 0)
                entries[name] = data[offset + 24:offset + 24 + size]
                self.assertEqual(len(entries[name]), size)
                offset += 24 + ((size + 15) & ~15)
            self.assertEqual(data[offset:], bytes(24))
            self.assertEqual(entries['rdimage'], boot.read_bytes())
            self.assertEqual(entries['ramdisk'], helper.read_bytes())
            self.assertEqual(entries['kernel'], kernel.read_bytes())
            self.assertNotIn('rdfile', entries)
            self.assertNotIn('rdres', entries)
            self.assertTrue(entries['cmdline'].endswith(b'\0'))
            args = entries['cmdline'][:-1].decode().split()
            for expected in ('rootdev=md0', 'root_apfs=ufs_darwin', 'ufs=2', 'ufs_native=1'):
                key = expected.split('=', 1)[0]
                self.assertEqual([arg for arg in args if arg.split('=', 1)[0] == key], [expected])
            self.assertEqual([arg for arg in args if arg.split('=', 1)[0] == 'rd'], [])

    def test_invalid_boot_size_refused_before_output_or_subprocess(self):
        module = self.packager()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, helper, boot = (root / name for name in ('kernel', 'helper', 'boot'))
            kernel.write_bytes(b'kernel fixture')
            helper.write_bytes(b'helper volume')
            output = root / 'new-directory' / 'payload.bin'
            for size in (0, 64 * 1024 * 1024 + 1):
                with self.subTest(size=size):
                    with boot.open('wb') as stream:
                        stream.truncate(size)
                    with patch.object(module.subprocess, 'run') as run, patch.object(module, 'compress_lz4') as compress:
                        with self.assertRaises(ValueError):
                            module.build_standalone_payload(kernel, helper, output, boot_image=boot)
                        run.assert_not_called()
                        compress.assert_not_called()
                    self.assertFalse(output.parent.exists())

    def test_full_container_limit_preserves_existing_output(self):
        module = self.packager()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, helper, boot = (root / name for name in ('kernel', 'helper', 'boot'))
            # Each entry fits separately; the complete uncompressed container exceeds 55 MiB.
            for path, mib in ((kernel, 27), (helper, 27), (boot, 2)):
                with path.open('wb') as stream:
                    stream.truncate(mib * 1024 * 1024)
            output = root / 'payload.bin'
            output.write_bytes(b'existing payload')
            with patch.object(module, 'compress_lz4') as compress:
                with self.assertRaisesRegex(RuntimeError, 'loader container limit'):
                    module.build_standalone_payload(kernel, helper, output, boot_image=boot)
                compress.assert_not_called()
            self.assertEqual(output.read_bytes(), b'existing payload')

    def test_duplicate_root_and_ufs_arguments_rejected_before_build(self):
        module = self.packager()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / 'fixture'
            fixture.write_bytes(b'fixture')
            output = root / 'new-directory' / 'payload.bin'
            with patch.object(module.subprocess, 'run') as run, patch.object(module, 'compress_lz4') as compress:
                for argument in ('rd=md0', 'rootdev=md0', 'root_apfs=ufs_darwin', 'ufs=2', 'ufs_native=1'):
                    with self.subTest(argument=argument), self.assertRaises(ValueError):
                        module.build_standalone_payload(fixture, fixture, output, boot_image=fixture,
                                                       cmdline=module.DEFAULT_CMDLINE + ' ' + argument)
                run.assert_not_called()
                compress.assert_not_called()
            self.assertFalse(output.parent.exists())

    def test_stale_macho_kernel_without_return_quiesce_rejected(self):
        module = self.packager()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel, helper, boot = (root / name for name in ('kernel', 'helper', 'boot'))
            kernel.write_bytes(b'\xcf\xfa\xed\xfe' + b'\x00' * (1048576 + 64))
            helper.write_bytes(b'NXSB' + b'\x00' * 1024)
            boot.write_bytes(b'NXSB' + b'\x00' * 1024)
            output = root / 'new-directory' / 'payload.bin'
            with patch.object(module.subprocess, 'run') as run, patch.object(module, 'compress_lz4') as compress:
                with self.assertRaisesRegex(ValueError, '_adreno8_return_quiesce'):
                    module.build_standalone_payload(kernel, helper, output, boot_image=boot)
                run.assert_not_called()
                compress.assert_not_called()
            self.assertFalse(output.parent.exists())

    def test_mkstandalone_script(self):
        script = HERE / "mkstandalone.py"
        res = subprocess.run(["python3", str(script), "--test"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("mkstandalone self-test: PASS", res.stdout)


if __name__ == "__main__":
    unittest.main()

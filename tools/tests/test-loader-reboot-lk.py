#!/usr/bin/env python3
"""Compile the loader's SPMI helper and LK reboot branch against an MMIO fixture."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile


def balanced_end(text, start, opening="{", closing="}"):
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError("unterminated source block")


def function(text, name):
    match = re.search(r"static (?:int|void)\n" + name + r"\(", text)
    if match is None:
        raise ValueError("missing actual function: " + name)
    brace = text.index("{", match.start())
    return text[match.start():balanced_end(text, brace)]


def sources(root):
    ui = (root / "xnuloader/loader_06a_screen_ui.inc").read_text()
    header = root / "xnuloader/loader_spmi_reboot_private.h"
    helper = function(header.read_text() if header.exists() else ui, "spmi_reboot_lk_prepare")
    observer = function(ui, "spmi_obsrv_read")
    usb = (root / "xnuloader/loader_06_usb_entry.inc").read_text()
    marker = 'if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {'
    start = usb.index(marker)
    branch = usb[start:balanced_end(usb, usb.index("{", start))]
    return observer, helper, branch


def model_mmio(text):
    """Change only volatile loads/stores; preserve the actual control flow."""
    token = "*(volatile uint32_t *)("
    while token in text:
        start = text.index(token)
        opening = start + len(token) - 1
        end = balanced_end(text, opening, "(", ")")
        address = text[opening + 1:end - 1]
        tail = end
        while tail < len(text) and text[tail].isspace():
            tail += 1
        if text[tail:tail + 1] == "=" and text[tail:tail + 2] != "==":
            semi = text.index(";", tail)
            value = text[tail + 1:semi].strip()
            text = text[:start] + f"mmio_write({address}, {value});" + text[semi + 1:]
        else:
            text = text[:start] + f"mmio_read({address})" + text[end:]
    return text


COMMON = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPMI_ARB_CORE ((uintptr_t)0x0c400000ULL)
#define SPMI_ARB_OBSRV ((uintptr_t)0x0c440000ULL)
static const char *case_name;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", case_name, __LINE__, #x); exit(1); \
} } while (0)

static struct { uint64_t fb_base; } platform_annibale = { 0xfc800000ULL }, other_platform;
static void *PLAT;
static struct { uint64_t base; } SCREEN;
static int kernel_resources_held, firmware_fault;
static struct {
    uint32_t version, channels, map[1024], owner[1024];
} fixture;
static struct {
    uint32_t version, channels, map[1024], owner[1024];
    uint32_t obs_status[2], obs_delay[2], obs_reads[2];
    uint32_t write_status, write_delay, write_reads;
    uint32_t obs_ops, write_commands, owner_reads, map_reads;
    uint8_t sdam, pending, readback, override_readback;
    unsigned resets, stalls, efi_resets, composed;
    char reply[8], message[96], result[40];
} model;

static uint32_t read_fixture_u32(FILE *file)
{
    uint8_t bytes[4];
    CHECK(fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void load_fixture(const char *path)
{
    case_name = "independent APID fixture";
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char magic[8];
    CHECK(fread(magic, 1, sizeof(magic), file) == sizeof(magic));
    CHECK(memcmp(magic, "SPMILK01", 8) == 0);
    fixture.version = read_fixture_u32(file);
    fixture.channels = read_fixture_u32(file);
    uint32_t entries = read_fixture_u32(file);
    CHECK(fixture.channels <= 1024 && entries <= 1024);
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t apid = read_fixture_u32(file);
        CHECK(apid < fixture.channels);
        fixture.map[apid] = read_fixture_u32(file);
        fixture.owner[apid] = read_fixture_u32(file);
    }
    CHECK(fgetc(file) == EOF);
    fclose(file);
}

static uint32_t mmio_read(uintptr_t address)
{
    if (address == 0x0c400000ULL) return model.version;
    if (address == 0x0c400004ULL) return model.channels;
    if (address >= 0x0c402000ULL && address < 0x0c403000ULL) {
        uint32_t apid = (uint32_t)((address - 0x0c402000ULL) / 4);
        CHECK((address & 3) == 0 && apid < model.channels);
        model.map_reads++;
        return model.map[apid];
    }
    if (address >= 0x0c42d000ULL && address < 0x0c42e000ULL) {
        uint32_t apid = (uint32_t)((address - 0x0c42d000ULL) / 4);
        CHECK((address & 3) == 0 && apid < model.channels);
        model.owner_reads++;
        return model.owner[apid];
    }
    if (address == 0x0c444948ULL) {
        CHECK(model.obs_ops > 0 && model.obs_ops <= 2);
        unsigned phase = model.obs_ops - 1;
        if (++model.obs_reads[phase] <= model.obs_delay[phase]) return 0;
        return model.obs_status[phase];
    }
    if (address == 0x0c444958ULL) {
        CHECK(model.obs_ops > 0 && model.obs_ops <= 2);
        if (model.obs_ops == 2 && model.override_readback) return model.readback;
        return model.sdam;
    }
    if (address == 0x0c74a008ULL) {
        CHECK(model.write_commands == 1);
        if (++model.write_reads <= model.write_delay) return 0;
        if ((model.write_status & 0xfu) == 1u) model.sdam = model.pending;
        return model.write_status;
    }
    CHECK(!"MMIO read outside APID 586 fixture");
    return 0;
}

static void mmio_write(uintptr_t address, uint32_t value)
{
    if (address == 0x0c444940ULL) {
        CHECK(value == 0x68000480u);
        CHECK(++model.obs_ops <= 2);
    } else if (address == 0x0c74a010ULL) {
        CHECK(value <= 255);
        model.pending = (uint8_t)value;
    } else if (address == 0x0c74a000ULL) {
        CHECK(value == 0x480u);
        CHECK(++model.write_commands == 1);
    } else {
        CHECK(!"MMIO write outside APID 586 fixture");
    }
}

static int starts_with(const char *value, const char *prefix)
{ return strncmp(value, prefix, strlen(prefix)) == 0; }
static int str_eq(const char *left, const char *right) { return strcmp(left, right) == 0; }
static void fb_reply(const char *tag, const char *message)
{
    snprintf(model.reply, sizeof(model.reply), "%s", tag);
    snprintf(model.message, sizeof(model.message), "%s", message);
}
static int64_t psci_reset_attempt(void) { model.resets++; return -7; }
static void print(const char *format, ...) { (void)format; }
static void set_result(const char *state, uint64_t first, uint64_t second)
{
    (void)first; (void)second;
    snprintf(model.result, sizeof(model.result), "%s", state);
}
static void compose_logs(const char *log, uint64_t length)
{ (void)log; (void)length; model.composed++; }
static void stall(uint64_t delay) { (void)delay; model.stalls++; }
static void efi_reset(int kind, uint64_t status, uint64_t size, void *data)
{ (void)kind; (void)status; (void)size; (void)data; model.efi_resets++; }
static struct { void (*stall)(uint64_t); } boot_services = { stall }, *BS = &boot_services;
static struct { void (*reset_system)(int, uint64_t, uint64_t, void *); } runtime = { efi_reset };
static struct { typeof(runtime) *rt; } system_table = { &runtime }, *ST = &system_table;

static void reset_model(const char *name, uint8_t original)
{
    case_name = name;
    memset(&model, 0, sizeof(model));
    model.version = fixture.version;
    model.channels = fixture.channels;
    memcpy(model.map, fixture.map, sizeof(model.map));
    memcpy(model.owner, fixture.owner, sizeof(model.owner));
    model.sdam = original;
    model.obs_status[0] = model.obs_status[1] = model.write_status = 1;
    SCREEN.base = platform_annibale.fb_base;
    PLAT = &platform_annibale;
    kernel_resources_held = firmware_fault = 0;
}
'''

CASES = r'''
static unsigned cases;
static void succeeded(uint8_t target)
{
    CHECK(model.resets == 1 && model.efi_resets == 0 && model.stalls == 0);
    CHECK(strcmp(model.reply, "OKAY") == 0);
    CHECK(strcmp(model.result, "reset-failed") == 0);
    CHECK(model.obs_ops == 2 && model.write_commands == 1);
    CHECK(model.pending == target && model.sdam == target);
    CHECK(model.owner_reads == 2 && model.map_reads == 587);
    cases++;
}
static void failed(const char *command)
{
    int hold = kernel_resources_held, fault = firmware_fault;
    invoke((char *)command);
    CHECK(model.resets == 0 && model.efi_resets == 0 && model.stalls == 0);
    CHECK(model.composed == 0 && model.result[0] == 0);
    CHECK(strcmp(model.reply, "FAIL") == 0);
    CHECK(strcmp(model.message, "LK reboot target preparation failed") == 0);
    CHECK(kernel_resources_held == hold && firmware_fault == fault);
    cases++;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    load_fixture(argv[1]);
    reset_model("skip APID 525 owner 3; use APID 586 owner 0", 0);
    invoke("reboot-bootloader"); succeeded(4);
    reset_model("oem reboot-lk preserves original bit 0", 0xab);
    invoke("oem reboot-lk"); succeeded(5);
    reset_model("discard original restart bits 1..7", 0xfe);
    invoke("reboot-bootloader"); succeeded(4);
    reset_model("owner field uses native bits 2..0", 0);
    model.owner[586] = 8;
    invoke("reboot-bootloader"); succeeded(4);
    reset_model("write completes at last permitted poll", 0);
    model.write_delay = 3999;
    invoke("reboot-bootloader"); succeeded(4);
    CHECK(model.write_reads == 4000);
    reset_model("observer completes at last permitted poll", 0);
    model.obs_delay[0] = model.obs_delay[1] = 3999;
    invoke("reboot-bootloader"); succeeded(4);
    CHECK(model.obs_reads[0] == 4000 && model.obs_reads[1] == 4000);

    const uint32_t bad_status[] = { 0, 2, 4, 8, 3, 5, 9, 0xffffffffu };
    for (unsigned i = 0; i < sizeof(bad_status) / sizeof(bad_status[0]); i++) {
        char name[96];
        snprintf(name, sizeof(name), "original read status %#x must not reset", bad_status[i]);
        reset_model(name, 4);
        model.obs_status[0] = bad_status[i];
        failed("reboot-bootloader");
        CHECK(model.write_commands == 0 && model.obs_ops == 1);
        if (!(bad_status[i] & 1)) CHECK(model.obs_reads[0] == 4000);
        snprintf(name, sizeof(name), "write status %#x must not reset", bad_status[i]);
        reset_model(name, 4);
        model.write_status = bad_status[i];
        failed("oem reboot-lk");
        CHECK(model.obs_ops == 1 && model.write_commands == 1);
        if (!(bad_status[i] & 1)) CHECK(model.write_reads == 4000);
        snprintf(name, sizeof(name), "readback status %#x must not reset", bad_status[i]);
        reset_model(name, 4);
        model.obs_status[1] = bad_status[i];
        failed("reboot-bootloader");
        CHECK(model.obs_ops == 2 && model.write_commands == 1);
    }
    reset_model("write completion after budget must not reset", 4);
    model.write_delay = 4000;
    failed("reboot-bootloader");
    CHECK(model.write_reads == 4000 && model.obs_ops == 1);
    reset_model("observer completion after budget must not reset", 4);
    model.obs_delay[0] = 4000;
    failed("reboot-bootloader");
    CHECK(model.obs_reads[0] == 4000 && model.write_commands == 0);
    reset_model("readback mismatches selected target", 0);
    model.override_readback = 1; model.readback = 6;
    failed("reboot-bootloader");
    reset_model("readback still has the old restart byte", 0);
    model.override_readback = 1; model.readback = 0;
    failed("oem reboot-lk");
    reset_model("no owner 0 among matching PPIDs", 0);
    model.owner[586] = 1;
    failed("reboot-bootloader");
    CHECK(model.obs_ops == 0 && model.write_commands == 0 && model.map_reads == 896);
    reset_model("APID 586 excluded by channel count", 0);
    model.channels = 586;
    failed("reboot-bootloader");
    CHECK(model.obs_ops == 0 && model.map_reads == 586);
    reset_model("unsupported arbiter version", 0);
    model.version = 0x60000000u;
    failed("reboot-bootloader");
    CHECK(model.map_reads == 0 && model.obs_ops == 0);
    reset_model("annibale display guard must not perform SPMI", 0);
    SCREEN.base = 0;
    failed("reboot-bootloader");
    CHECK(model.map_reads == 0 && model.obs_ops == 0);
    const char *lk[] = { "reboot-bootloader", "oem reboot-lk" };
    for (unsigned i = 0; i < 2; i++) {
        reset_model("other platform retains EFI LK reset behavior", 0);
        SCREEN.base = 0; PLAT = &other_platform;
        invoke((char *)lk[i]);
        CHECK(model.resets == 0 && model.efi_resets == 1 && model.stalls == 1);
        CHECK(strcmp(model.reply, "OKAY") == 0);
        CHECK(model.map_reads == 0 && model.obs_ops == 0 && model.write_commands == 0);
        cases++;
        reset_model("other platform held state retains direct PSCI behavior", 0);
        SCREEN.base = 0; PLAT = &other_platform;
        kernel_resources_held = firmware_fault = 1;
        invoke((char *)lk[i]);
        CHECK(model.resets == 1 && model.efi_resets == 0 && model.stalls == 0);
        CHECK(strcmp(model.reply, "OKAY") == 0);
        CHECK(model.map_reads == 0 && model.obs_ops == 0 && model.write_commands == 0);
        CHECK(kernel_resources_held == 1 && firmware_fault == 1);
        cases++;
    }
    reset_model("failure preserves held and firmware fault states", 0);
    kernel_resources_held = firmware_fault = 1;
    model.write_status = 3;
    failed("reboot-bootloader");

    const char *plain[] = { "reboot", "oem reboot" };
    for (unsigned i = 0; i < 2; i++) {
        reset_model("ordinary reboot retains direct PSCI behavior", 0);
        kernel_resources_held = firmware_fault = 1;
        invoke((char *)plain[i]);
        CHECK(model.resets == 1 && model.efi_resets == 0 && model.stalls == 0);
        CHECK(model.obs_ops == 0 && model.write_commands == 0 && model.map_reads == 0);
        CHECK(kernel_resources_held == 1 && firmware_fault == 1);
        cases++;
    }
    reset_model("observer rejects invalid request sizes", 0);
    uint8_t byte = 0;
    CHECK(!spmi_obsrv_read(-1, 0x48, &byte, 1));
    CHECK(!spmi_obsrv_read(586, 0x48, &byte, 0));
    CHECK(!spmi_obsrv_read(586, 0x48, &byte, 9));
    CHECK(model.obs_ops == 0);
    cases++;
    printf("loader LK reboot: %u cases passed\n", cases);
    return 0;
}
'''


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("mutation no longer matches actual source: " + old)
    return text.replace(old, new, 1)


def variants(observer, helper, branch):
    owner = "& 0x7u) != 0) continue;"
    write_guard = "if (!(status & 0x1u) || (status & 0xeu)) return 0;"
    original_guard = "if (!spmi_obsrv_read((int)n, 0x48, &orig, 1)) return 0;"
    readback = "return spmi_obsrv_read((int)n, 0x48, &readback, 1) && readback == (uint8_t)((orig & 1u) | 4u);"
    failure = '''if (!spmi_reboot_lk_prepare()) {
\t\t\t\t\tfb_reply("FAIL", "LK reboot target preparation failed");
\t\t\t\t\treturn;
\t\t\t\t}'''
    changes = [
        ("owner-first-match-break", helper, owner, "& 0x7u) != 0) break;", 1),
        ("owner-wrong-mask", helper, owner, "& 0xfu) != 0) continue;", 1),
        ("owner-check-removed", helper, owner, "& 0x7u) == 99u) continue;", 1),
        ("original-read-guard-removed", helper, original_guard,
         "(void)spmi_obsrv_read((int)n, 0x48, &orig, 1);", 1),
        ("write-done-check-removed", helper, write_guard, "if (status & 0xeu) return 0;", 1),
        ("write-error-check-removed", helper, write_guard, "if (!(status & 0x1u)) return 0;", 1),
        ("write-wait-shortened", helper, "i < 4000", "i < 3999", 1),
        ("readback-comparison-removed", helper, readback,
         "return spmi_obsrv_read((int)n, 0x48, &readback, 1);", 1),
        ("readback-removed", helper, readback, "return 1;", 1),
        ("restart-bit-preservation-changed", helper,
         "(uint32_t)((orig & 1u) | 4u)", "(uint32_t)(orig | 4u)", 1),
        ("channel-bound-removed", helper, "n < channels", "n < 1024", 1),
        ("observer-done-check-removed", observer, write_guard, "if (status & 0xeu) return 0;", 0),
        ("observer-error-check-removed", observer, write_guard, "if (!(status & 0x1u)) return 0;", 0),
        ("platform-scope-removed", branch, "PLAT == &platform_annibale &&\n\t\t\t    ", "", 2),
        ("caller-failure-check-removed", branch, failure, "spmi_reboot_lk_prepare();", 2),
        ("caller-failure-return-removed", branch, failure, failure.replace("\t\t\t\t\treturn;\n", ""), 2),
    ]
    for name, text, old, new, part in changes:
        pieces = [observer, helper, branch]
        pieces[part] = replace_once(text, old, new)
        yield name, tuple(pieces)


def execute(output, name, pieces, fixture):
    directory = output / name
    directory.mkdir(parents=True, exist_ok=True)
    observer, helper, branch = pieces
    code = COMMON + model_mmio(observer) + "\n" + model_mmio(helper)
    code += "\nstatic void invoke(char *cmd)\n{\n" + branch + "\n}\n" + CASES
    source = directory / "test.c"
    binary = directory / "test"
    source.write_text(code)
    compiler = shlex.split(os.environ.get("CC", "clang"))
    command = compiler + ["-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
                          "-Wno-unused-variable", str(source), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (directory / "compile.log").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError(f"{name} did not compile: {built.stderr[:1000]}")
    run = subprocess.run([str(binary), str(fixture)], capture_output=True, text=True, timeout=15)
    (directory / "result.log").write_text(run.stdout + run.stderr)
    return {"name": name, "returncode": run.returncode,
            "output": (run.stdout + run.stderr).strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--baseline-root", type=Path)
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="loader-reboot-lk-test-") as temporary:
        output = args.output_dir or Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        fixture = output / "apid-fixture.bin"
        # MMIO addresses and records are independent of the compiled helper.
        records = [(17, 0x00012300, 0), (525, 0x00007100, 3),
                   (586, 0x00007100, 0), (675, 0x00007100, 1)]
        fixture.write_bytes(b"SPMILK01" + struct.pack("<III", 0x70020000, 896, len(records)) +
                            b"".join(struct.pack("<III", *record) for record in records))
        actual = sources(args.source_root)
        results = [execute(output, "candidate", actual, fixture)]
        print(results[0]["output"])
        if results[0]["returncode"]:
            raise SystemExit("candidate failed")
        if args.baseline_root:
            baseline = execute(output, "baseline", sources(args.baseline_root), fixture)
            results.append(baseline)
            if baseline["returncode"] == 0:
                raise SystemExit("unfixed baseline unexpectedly passed")
            print("unfixed baseline rejected")
        if args.mutations:
            for name, pieces in variants(*actual):
                result = execute(output, name, pieces, fixture)
                results.append(result)
                if result["returncode"] == 0:
                    raise SystemExit("protection mutation passed: " + name)
            print(f"{len(results) - 1 - bool(args.baseline_root)} compiled protection mutations rejected")
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()

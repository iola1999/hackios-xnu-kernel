#!/usr/bin/env python3
"""Compile the production CMD_DB lookup against an independent binary fixture."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/dev/annibale/annibale_rpmh.c"

HARNESS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CMD_DB_PHYS 0x81c60000UL
#define CMD_DB_SIZE 0x20000UL
typedef uintptr_t vm_offset_t;
static uint8_t database[CMD_DB_SIZE];
static int annibale_rpmh_lock, lock_depth;
static void require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
static void lck_mtx_lock(int *lock) {
    require(lock == &annibale_rpmh_lock && lock_depth == 0, "lock entry");
    lock_depth++;
}
static void lck_mtx_unlock(int *lock) {
    require(lock == &annibale_rpmh_lock && lock_depth == 1, "lock exit");
    lock_depth--;
}
static vm_offset_t ml_io_map_wcomb(vm_offset_t address, size_t length) {
    require(address == CMD_DB_PHYS && length == CMD_DB_SIZE, "mapping bounds");
    return (vm_offset_t)database;
}
__STRUCTS__
static const struct cmd_db_header *cmd_db_va;
__LOOKUP__
int main(int argc, char **argv) {
    require(argc == 2, "fixture argument");
    FILE *input = fopen(argv[1], "rb");
    require(input != NULL, "fixture open");
    require(fread(database, 1, 512, input) == 512, "fixture size");
    require(fclose(input) == 0, "fixture close");
    const uint8_t levels[] = {0, 0, 128, 0, 0, 1, 128, 1};
    const uint8_t *data = NULL;
    uint32_t length = 0;
    require(annibale_cmd_db_lookup("mmcx.lvl", &data, &length) == 0x30020,
            "full-width mmcx.lvl must match all eight bytes");
    require(data == database + 144 + 128 && length == sizeof(levels), "ARC metadata");
    require(memcmp(data, levels, sizeof(levels)) == 0, "ARC levels");
    require(annibale_cmd_db_lookup("mxc.lvl", &data, &length) == 0x30030,
            "short ID must match zero padding");
    require(data == database + 144 + 128 + 8 && length == sizeof(levels), "short metadata");
    require(annibale_cmd_db_lookup("mmcx.lv", NULL, NULL) == 0, "short prefix must not match");
    require(annibale_cmd_db_lookup("mmcx.lvlx", NULL, NULL) == 0, "long prefix must not match");
    require(annibale_cmd_db_lookup(NULL, NULL, NULL) == 0, "NULL name");
    require(annibale_cmd_db_lookup("", NULL, NULL) == 0, "empty name");
    require(annibale_cmd_db_lookup("missing", NULL, NULL) == 0, "unknown name");
    database[4] = 0;
    require(annibale_cmd_db_lookup("mxc.lvl", NULL, NULL) == 0, "invalid magic");
    require(lock_depth == 0, "lookup must release lock");
    return 0;
}
'''


def binary_fixture():
    # Linux CMD_DB: eight 16-byte slave descriptors follow the 8-byte prefix.
    result = bytearray(512)
    struct.pack_into("<I4s", result, 0, 1, bytes.fromhex("db30030c"))
    struct.pack_into("<HHHHHHHH", result, 8, 3, 64, 128, 2, 1, 0, 0, 0)
    for index, (key, address) in enumerate(((b"mmcx.lvl", 0x30020), (b"mxc.lvl\0", 0x30030))):
        struct.pack_into("<8sIIIHH", result, 144 + 64 + index * 24,
                         key, 0, 0, address, 8, index * 8)
        struct.pack_into("<4H", result, 144 + 128 + index * 8, 0, 128, 256, 384)
    return result


def main():
    source = SOURCE.read_text()
    structs = source[source.index("struct cmd_db_slave {"):source.index("static LCK_GRP_DECLARE")]
    lookup = source[source.index("uint32_t\nannibale_cmd_db_lookup"):source.index("bool\nannibale_rpmh_tcs_send")]
    with tempfile.TemporaryDirectory(prefix="annibale-cmd-db-") as folder:
        base = Path(folder)
        fixture = base / "cmd-db.bin"
        fixture.write_bytes(binary_fixture())

        def execute(label, body):
            unit = base / f"{label}.c"
            unit.write_text(HARNESS.replace("__STRUCTS__", structs).replace("__LOOKUP__", body))
            binary = base / label
            compile_result = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                             "-Wno-tautological-constant-out-of-range-compare",
                                             str(unit), "-o", str(binary)], capture_output=True, text=True)
            if compile_result.returncode:
                raise SystemExit(compile_result.stderr)
            return subprocess.run([str(binary), str(fixture)], capture_output=True, text=True)

        result = execute("production", lookup)
        if result.returncode:
            raise SystemExit(result.stderr)
        copy = "memcpy(id, name, name_len);"
        if lookup.count(copy) != 1:
            raise SystemExit("production fixed-width copy was not found")
        regression = execute("truncated", lookup.replace(copy, "strlcpy(id, name, sizeof(id));"))
        if regression.returncode == 0 or "full-width mmcx.lvl" not in regression.stderr:
            raise SystemExit("the fixture did not reject the former strlcpy truncation")
    print("PASS: CMD_DB full-width/short IDs, long prefixes, metadata, and invalid inputs")
    print("PASS: the compiled former strlcpy truncation fails the mmcx.lvl lookup")


if __name__ == "__main__":
    main()

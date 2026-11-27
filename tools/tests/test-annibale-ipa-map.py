#!/usr/bin/env python3
"""Host ASan+UBSan unit tests for bounded/chunked carveout mapping in annibale_ipa.c."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IPA_C = ROOT / "xnu/files/bsd/dev/annibale/annibale_ipa.c"
ipa_src = IPA_C.read_text()


def extract_c_function(src: str, signature_prefix: str) -> str:
    idx = src.find(signature_prefix)
    if idx < 0:
        raise AssertionError(f"missing {signature_prefix!r} in {IPA_C}")
    line_start = src.rfind("\n", 0, idx)
    ret_start = src.rfind("\n", 0, line_start)
    start = 0 if ret_start < 0 else ret_start + 1
    brace = src.find("{", idx)
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[start : pos + 1]
    raise AssertionError(f"unbalanced braces for {signature_prefix!r}")


assert "ml_io_map_unmappable(mem_phys, region_size" not in ipa_src
assert "#define IPA_CARVEOUT_MAP_CHUNK" in ipa_src

helpers_src = "\n\n".join([
    extract_c_function(ipa_src, "ipa_wcomb_copy("),
    extract_c_function(ipa_src, "ipa_wcomb_zero("),
    extract_c_function(ipa_src, "ipa_carveout_map_range("),
    extract_c_function(ipa_src, "ipa_carveout_copy("),
    extract_c_function(ipa_src, "ipa_carveout_zero("),
    extract_c_function(ipa_src, "ipa_carveout_read32("),
    extract_c_function(ipa_src, "annibale_load_mdt_image("),
])

harness = f'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;

#define PAGE_SHIFT 14u
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1u)
#define VM_WIMG_WCOMB 2u
#define SCM_PAS_MEM_SETUP 0x0202u
#define IPA_TZ_HDR_SLOTS 3u
#define IPA_TZ_HDR_SLOT_SZ 0x10000u
#define IPA_CARVEOUT_MAP_CHUNK  0x00400000ULL

struct ipa_elf32_phdr {{
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
}};

#define MOCK_BASE 0x8bc00000ULL
#define MOCK_CAP  0x01000000ULL
#define MOCK_REGION_REPORTED 0x0f600000ULL

static uint8_t *mock_carveout;
static uint8_t mock_hdr[IPA_TZ_HDR_SLOTS][IPA_TZ_HDR_SLOT_SZ];
static vm_offset_t ipa_tz_hdr_va[IPA_TZ_HDR_SLOTS];
static uint64_t ipa_tz_hdr_pa[IPA_TZ_HDR_SLOTS];

struct mock_file {{
    char name[32];
    const uint8_t *data;
    uint32_t size;
    bool is_override;
}};
static struct mock_file mock_files[8];
static unsigned mock_file_count;

static uint64_t active_map_pa, max_map_sz, total_mapped_bytes;
static size_t active_map_sz;
static bool map_open;
static unsigned map_calls, unmap_calls, fail_on_map_call;

static bool ipa_ensure_tz_hdr_pages(void) {{
    for (unsigned s = 0; s < IPA_TZ_HDR_SLOTS; s++) {{
        ipa_tz_hdr_va[s] = (vm_offset_t)mock_hdr[s];
        ipa_tz_hdr_pa[s] = 0xcf6fc000ULL + (uint64_t)s * IPA_TZ_HDR_SLOT_SZ;
    }}
    return true;
}}
static int64_t pas_init_try(uint32_t pas, uint64_t hdr_pa, uint64_t *res1) {{
    (void)pas; (void)hdr_pa; *res1 = 0; return 0;
}}
static int64_t annibale_tz_smc(uint64_t fn, uint64_t ai, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t *res1_out) {{
    (void)fn; (void)ai; (void)a0; (void)a1; (void)a2; (void)a3;
    *res1_out = 0;
    return 0;
}}

static vm_offset_t ml_io_map_unmappable(vm_offset_t pa, vm_size_t sz, unsigned int fl) {{
    assert(fl == VM_WIMG_WCOMB);
    assert(!map_open);
    assert((pa & PAGE_MASK) == 0 && (sz & PAGE_MASK) == 0 && sz > 0);
    map_calls++;
    if (fail_on_map_call != 0 && map_calls == fail_on_map_call) {{
        return 0;
    }}
    assert(pa >= MOCK_BASE && (pa - MOCK_BASE) + sz <= MOCK_CAP);
    assert(sz <= IPA_CARVEOUT_MAP_CHUNK + PAGE_SIZE);
    map_open = true;
    active_map_pa = pa;
    active_map_sz = sz;
    if (sz > max_map_sz) max_map_sz = sz;
    total_mapped_bytes += sz;
    return (vm_offset_t)(mock_carveout + (pa - MOCK_BASE));
}}

static void ml_io_unmap(vm_offset_t va, vm_size_t sz) {{
    assert(map_open);
    assert(va == (vm_offset_t)(mock_carveout + (active_map_pa - MOCK_BASE)));
    assert(sz == active_map_sz);
    map_open = false;
    unmap_calls++;
}}

static uint8_t *annibale_read_fw_file(const char *name, uint64_t *sz, bool *need_free) {{
    for (unsigned i = 0; i < mock_file_count; i++) {{
        if (strcmp(mock_files[i].name, name) == 0) {{
            uint8_t *cp = malloc(mock_files[i].size);
            assert(cp != NULL);
            memcpy(cp, mock_files[i].data, mock_files[i].size);
            *sz = mock_files[i].size;
            *need_free = true;
            return cp;
        }}
    }}
    return NULL;
}}

static uint8_t *annibale_read_fw_override(const char *name, uint64_t *sz, bool *need_free) {{
    for (unsigned i = 0; i < mock_file_count; i++) {{
        if (mock_files[i].is_override && strcmp(mock_files[i].name, name) == 0) {{
            uint8_t *cp = malloc(mock_files[i].size);
            assert(cp != NULL);
            memcpy(cp, mock_files[i].data, mock_files[i].size);
            *sz = mock_files[i].size;
            *need_free = true;
            return cp;
        }}
    }}
    return NULL;
}}

static bool annibale_ufs_fat16_fw_read(const char *name, uint64_t *sz, volatile void *dst, uint64_t cap) {{
    for (unsigned i = 0; i < mock_file_count; i++) {{
        if (!mock_files[i].is_override && strcmp(mock_files[i].name, name) == 0) {{
            assert(mock_files[i].size <= cap);
            for (uint32_t j = 0; j < mock_files[i].size; j++) {{
                ((volatile uint8_t *)dst)[j] = mock_files[i].data[j];
            }}
            *sz = mock_files[i].size;
            return true;
        }}
    }}
    return false;
}}

static void kfree_data(void *p, uint64_t sz) {{ (void)sz; free(p); }}

{helpers_src}

static void build_elf(uint8_t *mdt, uint32_t e_phoff, const struct ipa_elf32_phdr *phs, uint16_t count) {{
    memset(mdt, 0, e_phoff + count * sizeof(*phs));
    mdt[0] = 0x7f; mdt[1] = 'E'; mdt[2] = 'L'; mdt[3] = 'F';
    mdt[4] = 1; mdt[5] = 1; mdt[6] = 1;
    memcpy(mdt + 28, &e_phoff, 4);
    uint16_t phentsz = 32;
    memcpy(mdt + 42, &phentsz, 2);
    memcpy(mdt + 44, &count, 2);
    memcpy(mdt + e_phoff, phs, count * sizeof(*phs));
}}

static void reset_env(void) {{
    memset(mock_carveout, 0x5a, MOCK_CAP);
    mock_file_count = 0;
    map_open = false;
    map_calls = 0;
    unmap_calls = 0;
    max_map_sz = 0;
    total_mapped_bytes = 0;
    fail_on_map_call = 0;
}}

int main(void) {{
    mock_carveout = calloc(1, MOCK_CAP);
    assert(mock_carveout != NULL);

    /* Case 1: Sub-16 KiB unaligned segment, >4 MiB override copy, >4 MiB BSS, and tail zeroing in 246 MiB region. */
    reset_env();
    uint32_t seg1_off = 0x1240u;
    uint32_t seg1_filesz = 0x500u;
    uint32_t seg1_memsz = 0x5000u;
    uint32_t seg2_off = 0x10400u;
    uint32_t seg2_filesz = 0x480000u; /* 4.5 MiB: crosses IPA_CARVEOUT_MAP_CHUNK */
    uint32_t seg2_memsz = 0xa00000u;  /* +5.5 MiB BSS: crosses chunk twice */

    uint8_t mdt1[52 + 3 * 32];
    struct ipa_elf32_phdr phs1[3] = {{
        {{ .type = 1, .paddr = (uint32_t)MOCK_BASE, .filesz = 52 + 3 * 32, .memsz = 0x1000, .flags = 0 }},
        {{ .type = 1, .paddr = (uint32_t)MOCK_BASE + seg1_off, .filesz = seg1_filesz, .memsz = seg1_memsz, .flags = 0 }},
        {{ .type = 1, .paddr = (uint32_t)MOCK_BASE + seg2_off, .filesz = seg2_filesz, .memsz = seg2_memsz, .flags = 0 }},
    }};
    build_elf(mdt1, 52, phs1, 3);

    uint8_t *b00 = malloc(phs1[0].filesz);
    uint8_t *b01 = malloc(seg1_filesz);
    uint8_t *b02 = malloc(seg2_filesz);
    assert(b00 && b01 && b02);
    for (uint32_t i = 0; i < phs1[0].filesz; i++) b00[i] = (uint8_t)(0x10u + (i & 0x3fu));
    b00[0] = 0x41; b00[1] = 0x42; b00[2] = 0x43; b00[3] = 0x44;
    memset(b01, 0x3c, seg1_filesz);
    for (uint32_t i = 0; i < seg2_filesz; i++) b02[i] = (uint8_t)(0x80u ^ (i & 0x7fu));

    strcpy(mock_files[0].name, "modem.mdt"); mock_files[0].data = mdt1; mock_files[0].size = sizeof(mdt1); mock_files[0].is_override = false;
    strcpy(mock_files[1].name, "modem.b00"); mock_files[1].data = b00; mock_files[1].size = phs1[0].filesz; mock_files[1].is_override = false;
    strcpy(mock_files[2].name, "modem.b01"); mock_files[2].data = b01; mock_files[2].size = seg1_filesz; mock_files[2].is_override = false;
    strcpy(mock_files[3].name, "modem.b02"); mock_files[3].data = b02; mock_files[3].size = seg2_filesz; mock_files[3].is_override = true;
    mock_file_count = 4;

    uint32_t used_pas = 99;
    assert(annibale_load_mdt_image("modem", 4, 0, MOCK_BASE, MOCK_REGION_REPORTED, &used_pas));
    assert(used_pas == 4);
    assert(!map_open && map_calls == unmap_calls && map_calls >= 6);
    assert(max_map_sz <= IPA_CARVEOUT_MAP_CHUNK + PAGE_SIZE);
    assert(memcmp(mock_carveout, b00, phs1[0].filesz) == 0);
    assert(mock_carveout[seg1_off - 1] == 0x5a);
    assert(memcmp(mock_carveout + seg1_off, b01, seg1_filesz) == 0);
    for (uint32_t z = seg1_off + seg1_filesz; z < seg1_off + seg1_memsz; z++) assert(mock_carveout[z] == 0);
    assert(mock_carveout[seg1_off + seg1_memsz] == 0x5a);
    assert(memcmp(mock_carveout + seg2_off, b02, seg2_filesz) == 0);
    for (uint32_t z = seg2_off + seg2_filesz; z < seg2_off + seg2_memsz; z++) assert(mock_carveout[z] == 0);
    uint32_t highest = seg2_off + seg2_memsz;
    uint32_t span = (highest + 0xfffu) & ~0xfffu;
    for (uint32_t z = highest; z < span; z++) assert(mock_carveout[z] == 0);

    /* Case 2: Map failure mid-way must unmap/free cleanly without leaks. */
    reset_env();
    mock_file_count = 4;
    fail_on_map_call = 3;
    assert(!annibale_load_mdt_image("modem", 4, 0, MOCK_BASE, MOCK_REGION_REPORTED, &used_pas));
    assert(!map_open && map_calls == 3 && unmap_calls == 2);

    free(b00);
    free(b01);
    free(b02);
    free(mock_carveout);
    printf("PASS: annibale_ipa chunked page-aligned carveout map tests (ASan+UBSan)\\n");
    return 0;
}}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(harness)
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(src), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)

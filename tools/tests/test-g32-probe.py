#!/usr/bin/env python3
"""Host ASan+UBSan unit test for annibalemetal_g32_probe.inc (ANNIBALE_G32_PROBE).

Directly compiles annibalemetal_g32_probe.inc with -Wall -Wextra -Werror -fsanitize=address,undefined
and verifies:
  1. Default-off (unset or "0"): zero output and rw_g32_probe_limit() == 0.
  2. Non-matching pipelines (e.g. VfxU10Xh + TimgA2Xhfs_Isrc): zero output even when enabled.
  3. Matching VfxLlnXh + TcmaA2LlnXhfs draw (direct and indexed): formats pipeline names,
     primitive/count/index, blend/mask/scissor/viewport, ubo1[0x94] fp16 value, and first 4
     vertices of attr0 (float4) and attr3 (half4) with bounds checks.
  4. Rate limit (ANNIBALE_G32_PROBE=2) caps output at exactly 2 lines.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRV_DIR = ROOT / "tools/userland-driver"

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANNIBALE_G32_PROBE_HOST_TEST 1
#include "annibalemetal_g32_probe.inc"

static void reset_probe_state(void) {
    rw_g32_limit_cache = -1;
    rw_g32_emitted = 0;
}

static void make_sample_draw(struct rw_g32_draw_info *d,
                             uint8_t *vbuf, size_t vbuf_len,
                             uint8_t *ubo1, size_t ubo1_len,
                             uint16_t *ibuf, size_t ibuf_len) {
    memset(d, 0, sizeof(*d));
    memset(vbuf, 0, vbuf_len);
    memset(ubo1, 0, ubo1_len);

    /* 4 vertices: stride 24 = float4 position (16 B) + half4 color (8 B) */
    for (unsigned i = 0; i < 4; i++) {
        float pos[4] = { (float)(i * 10), (float)(i * 20), 0.0f, 1.0f };
        uint16_t col[4] = { 0x3c00u, 0x3800u, 0x0000u, 0x3c00u }; /* (1.0, 0.5, 0.0, 1.0) */
        memcpy(vbuf + i * 24u, pos, 16u);
        memcpy(vbuf + i * 24u + 16u, col, 8u);
        ibuf[i] = (uint16_t)(3u - i);
    }
    uint16_t half_one = 0x3c00u; /* 1.0h */
    memcpy(ubo1 + 0x94u, &half_one, 2u);

    d->vname = "VfxLlnXh";
    d->fname = "TcmaA2LlnXhfs";
    d->label = "com.apple.coreanimation.Lln";
    d->primitive = 4u; /* TriangleStrip */
    d->vertex_start = 0u;
    d->vertex_count = 4u;
    d->blend_enabled = 1u;
    d->write_mask = 0xfu;
    d->src_rgb = 1u; d->dst_rgb = 5u; d->op_rgb = 0u;
    d->src_a = 1u; d->dst_a = 5u; d->op_a = 0u;
    d->has_scissor = 1;
    d->scissor[0] = 0; d->scissor[1] = 0; d->scissor[2] = 1206; d->scissor[3] = 2622;
    d->has_viewport = 1;
    d->viewport[0] = 0; d->viewport[1] = 0; d->viewport[2] = 1206; d->viewport[3] = 2622;
    d->ubo1_bytes = ubo1;
    d->ubo1_len = ubo1_len;
    d->ubo1_offset = 0;
    d->attr0 = (struct rw_g32_vattr_meta){ 31u, 0u, 0u, 24u };
    d->attr0_bytes = vbuf; d->attr0_len = vbuf_len; d->attr0_buf_off = 0;
    d->attr3 = (struct rw_g32_vattr_meta){ 27u, 16u, 0u, 24u };
    d->attr3_bytes = vbuf; d->attr3_len = vbuf_len; d->attr3_buf_off = 0;
    (void)ibuf_len;
}

int main(void) {
    uint8_t vbuf[24 * 4], ubo1[256];
    uint16_t ibuf[4];
    struct rw_g32_draw_info d;
    char line[640];

    /* 1. Default-off (unset and "0") */
    unsetenv("ANNIBALE_G32_PROBE");
    reset_probe_state();
    make_sample_draw(&d, vbuf, sizeof(vbuf), ubo1, sizeof(ubo1), ibuf, sizeof(ibuf));
    assert(rw_g32_probe_limit() == 0);
    assert(rw_g32_format_line(line, sizeof(line), &d) == 0);

    setenv("ANNIBALE_G32_PROBE", "0", 1);
    reset_probe_state();
    assert(rw_g32_probe_limit() == 0);
    assert(rw_g32_format_line(line, sizeof(line), &d) == 0);
    printf("  [PASS] default-off (unset / 0) emits nothing\n");

    /* 2. Enabled ("1"): non-matching pipeline emits nothing; matching pipeline formats fields */
    setenv("ANNIBALE_G32_PROBE", "1", 1);
    reset_probe_state();
    assert(rw_g32_probe_limit() == 64);

    struct rw_g32_draw_info other = d;
    other.vname = "VfxU10Xh";
    other.fname = "TimgA2Xhfs_Isrc";
    other.label = "com.apple.coreanimation.Timg";
    assert(rw_g32_format_line(line, sizeof(line), &other) == 0);

    /* Line draws (prim=1, e.g. status-bar battery) must be skipped without consuming quota */
    struct rw_g32_draw_info line_draw = d;
    line_draw.primitive = 1u;
    assert(rw_g32_format_line(line, sizeof(line), &line_draw) == 0);
    assert(rw_g32_emitted == 0u);

    assert(rw_g32_format_line(line, sizeof(line), &d) == 1);
    assert(strstr(line, "g32-probe #0 v=VfxLlnXh f=TcmaA2LlnXhfs") != NULL);
    assert(strstr(line, "prim=4 start=0 count=4 idx=none(0)") != NULL);
    assert(strstr(line, "blend=1 mask=0xf rgb=1/5/0 a=1/5/0") != NULL);
    assert(strstr(line, "sc=0,0,1206,2622 vp=0,0,1206,2622") != NULL);
    assert(strstr(line, "ubo1_94=0x3c00(1.000)") != NULL);
    assert(strstr(line, "attr0=fmt=31/b0+0/s24[0.0,0.0,0.0,1.0;10.0,20.0,0.0,1.0;") != NULL);
    assert(strstr(line, "attr3=fmt=27/b0+16/s24[3c00,3800,0000,3c00;") != NULL);

    /* Indexed draw reads reversed vertex 3 first (30.0, 60.0) */
    d.indexed = 1;
    d.index_type = 0;
    d.index_bytes = (const uint8_t *)ibuf;
    d.index_len = sizeof(ibuf);
    d.index_offset = 0;
    assert(rw_g32_format_line(line, sizeof(line), &d) == 1);
    assert(strstr(line, "idx=u16(0)") != NULL);
    assert(strstr(line, "attr0=fmt=31/b0+0/s24[30.0,60.0,0.0,1.0;") != NULL);
    printf("  [PASS] matching draw (direct + indexed) formats all fields\n");

    /* 3. Rate limit + out-of-bounds buffer safety */
    setenv("ANNIBALE_G32_PROBE", "2", 1);
    reset_probe_state();
    d.ubo1_len = 0x40; /* too short for 0x94 */
    d.attr3_len = 8;   /* too short for vertex offset 16 */
    assert(rw_g32_format_line(line, sizeof(line), &d) == 1);
    assert(strstr(line, "ubo1_94=none") != NULL);
    assert(strstr(line, "oob") != NULL);
    assert(rw_g32_format_line(line, sizeof(line), &d) == 1);
    assert(rw_g32_format_line(line, sizeof(line), &d) == 0);
    printf("  [PASS] rate limit + OOB buffer guards\n");

    printf("G32 probe host tests: PASS (3/3)\n");
    return 0;
}
'''


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="g32-probe-test-") as td:
        src = Path(td) / "test_g32_probe.c"
        exe = Path(td) / "test_g32_probe"
        src.write_text(HARNESS)
        cmd = [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(DRV_DIR),
            str(src),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        subprocess.run([str(exe)], check=True, env=env)
    return 0


if __name__ == "__main__":
    sys.exit(main())

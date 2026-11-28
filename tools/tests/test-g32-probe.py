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
  5. 256x1 RGBA16Float buffer-backed LUT sampling (rw_g32_format_buf_tex) with NaN/Inf count
     and short-buffer OOB guard, plus multi-slot texture scanning (t3=256x1 LUT + t9=noise_tex),
     ring-buffer fbuf5 (length > 16 + non-zero offset), and draw-time LUT buffer sampling.
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
    rw_g32_filter_init = 0;
    rw_g32_filter_cache = NULL;
    rw_g32_emitted = 0;
    rw_g32_tex_emitted = 0;
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
    char line[1536];

    /* 1. Default-off (unset and "0") */
    unsetenv("ANNIBALE_G32_PROBE");
    unsetenv("ANNIBALE_G32_FILTER");
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

    /* 4. 256x1 RGBA16Float LUT texture create/write/buf-tex + NaN/Inf detection */
    setenv("ANNIBALE_G32_PROBE", "1", 1);
    reset_probe_state();
    assert(rw_g32_format_tex_create(line, sizeof(line), 80u, 2u, 1206u, 2622u, 1) == 0);
    assert(rw_g32_format_tex_create(line, sizeof(line), 115u, 2u, 256u, 1u, 0) == 1);
    assert(strstr(line, "g32-tex-new #0 fmt=115 type=2 256x1 surf=0") != NULL);

    uint16_t lut[256 * 4];
    memset(lut, 0, sizeof(lut));
    lut[0] = 0x3c00u; lut[1] = 0x3800u; lut[2] = 0x0000u; lut[3] = 0x3c00u;
    lut[10 * 4 + 0] = 0x7e00u; /* NaN */
    lut[11 * 4 + 1] = 0x7c00u; /* +Inf */
    lut[12 * 4 + 2] = 0xfc00u; /* -Inf */
    lut[128 * 4 + 0] = 0x3800u; lut[128 * 4 + 3] = 0x3c00u;
    lut[255 * 4 + 2] = 0x3c00u; lut[255 * 4 + 3] = 0x3c00u;
    assert(rw_g32_format_tex_write(line, sizeof(line), 115u, 256u, 1u, 256u, 1u,
                                   lut, 2048u, 0) == 1);
    assert(strstr(line, "g32-tex-wr #1 fmt=115 tex=256x1 wr=256x1 stride=2048 rc=0 nz=6/256 nan=3") != NULL);
    assert(strstr(line, "nz0=(1.000,0.500,0.000,1.000)") != NULL);

    /* Buffer-backed 256x1 fmt=115 LUT sampling (newTextureWithDescriptor:offset:bytesPerRow:) */
    assert(rw_g32_format_buf_tex(line, sizeof(line), 80u, 512u, 512u, lut, sizeof(lut), 0u, 2048u, 1) == 0);
    assert(rw_g32_format_buf_tex(line, sizeof(line), 115u, 256u, 1u, lut, sizeof(lut), 0u, 2048u, 1) == 1);
    assert(strstr(line, "g32-buf-tex #2 fmt=115 tex=256x1 off=0/2048 stride=2048 ok=1 nz=6/256 nan=3") != NULL);
    assert(strstr(line, "nz0=(1.000,0.500,0.000,1.000)") != NULL);
    assert(strstr(line, "mid=(0.500,0.000,0.000,1.000)") != NULL);
    assert(strstr(line, "last=(0.000,0.000,1.000,1.000)") != NULL);

    /* Short/OOB buffer guard on rw_g32_format_buf_tex */
    assert(rw_g32_format_buf_tex(line, sizeof(line), 115u, 256u, 1u, lut, 64u, 0u, 2048u, 1) == 1);
    assert(strstr(line, "nz=1/8 nan=0") != NULL);
    assert(rw_g32_format_buf_tex(line, sizeof(line), 115u, 256u, 1u, lut, 64u, 128u, 2048u, 0) == 1);
    assert(strstr(line, "ok=0 nz=0/0 nan=0") != NULL);
    printf("  [PASS] 256x1 RGBA16Float tex-wr & buf-tex LUT sampling + NaN/Inf + OOB guard\n");

    /* 5. Multi-slot texture matching ([[texture(3)]] LUT + [[texture(9)]] noise_tex) + ring fbuf5 */
    uint8_t ring[4096];
    memset(ring, 0, sizeof(ring));
    uint16_t noise_scale = 0x3c00u; /* 1.0h */
    memcpy(ring + 0x104u, &noise_scale, 2u);

    struct rw_g32_draw_info timg = other;
    timg.fname = "TimgBdsoA2Xhfn_Isrc";
    timg.tex_count = 10u;
    timg.tex[3] = (struct rw_g32_tex_meta){ 115u, 256u, 1u };
    timg.tex[9] = (struct rw_g32_tex_meta){ 70u, 128u, 128u };
    timg.has_lut_buf = 1;
    timg.lut_slot = 3u;
    timg.lut_fmt = 115u;
    timg.lut_w = 256u;
    timg.lut_bytes = (const uint8_t *)lut;
    timg.lut_len = sizeof(lut);
    timg.lut_offset = 0u;
    timg.fbuf5_bytes = ring;
    timg.fbuf5_len = sizeof(ring); /* > 16: QuartzCore ring buffer */
    timg.fbuf5_offset = 0x104u;
    timg.attr1 = (struct rw_g32_vattr_meta){ 29u, 0u, 0u, 24u };
    timg.attr1_bytes = vbuf; timg.attr1_len = sizeof(vbuf); timg.attr1_buf_off = 0;
    assert(rw_g32_format_line(line, sizeof(line), &timg) == 1);
    assert(strstr(line, "f=TimgBdsoA2Xhfn_Isrc") != NULL);
    assert(strstr(line, "tex=t3=fmt115:256x1,t9=fmt70:128x128") != NULL);
    assert(strstr(line, "fbuf5=len4096+off260:0x3c00(1.0000)") != NULL);
    assert(strstr(line, "lut=t3:nz=6/256,nan=3,nz0=(1.000,0.500,0.000,1.000)") != NULL);
    assert(strstr(line, "attr1=fmt=29/b0+0/s24[") != NULL);

    /* Timg* with only small fbuf5 (noiseScale) and non-LUT tex0 still matches */
    struct rw_g32_draw_info fbuf5_only = other;
    fbuf5_only.tex_count = 1u;
    fbuf5_only.tex[0] = (struct rw_g32_tex_meta){ 80u, 512u, 512u };
    fbuf5_only.fbuf5_bytes = (const uint8_t *)&noise_scale;
    fbuf5_only.fbuf5_len = 2u;
    fbuf5_only.fbuf5_offset = 0u;
    assert(rw_g32_format_line(line, sizeof(line), &fbuf5_only) == 1);
    assert(strstr(line, "tex=t0=fmt80:512x512 fbuf2=none fbuf4=none fbuf5=len2+off0:0x3c00(1.0000)") != NULL);
    printf("  [PASS] multi-slot texture (t3/t9) + ring-buffer fbuf5 + draw-time LUT sampling\n");

    /* 6. ANNIBALE_G32_FILTER + fbuf2 (lod_bias) + fbuf4 (21 half4 entries + OOB guard) */
    setenv("ANNIBALE_G32_PROBE", "4", 1);
    setenv("ANNIBALE_G32_FILTER", "N3Oc3mtc4nlnlnl", 1);
    reset_probe_state();
    /* Lockscreen draw with 256x1 fmt=115 LUT must be skipped when filter does not match */
    assert(rw_g32_format_line(line, sizeof(line), &timg) == 0);
    assert(rw_g32_emitted == 0u);

    char long_line[2048];
    float lod_bias = -0.5f;
    uint16_t cp_ring[128]; /* 256 bytes */
    memset(cp_ring, 0, sizeof(cp_ring));
    /* Put 21 half4 entries starting at byte offset 16 (index 8 in uint16_t); mark entry 20 distinctly */
    for (unsigned i = 0; i < 21u; i++) {
        cp_ring[8u + i * 4u + 0u] = (i == 20u) ? 0x4000u : 0x3c00u; /* 2.0 on #20, 1.0 elsewhere */
        cp_ring[8u + i * 4u + 1u] = 0x3800u; /* 0.5 */
        cp_ring[8u + i * 4u + 2u] = 0x0000u; /* 0.0 */
        cp_ring[8u + i * 4u + 3u] = 0x3c00u; /* 1.0 */
    }
    struct rw_g32_draw_info cprog = timg;
    cprog.fname = "TimgA2Xhfn_IsrcN3Oc3mtc4nlnlnl";
    cprog.fbuf2_bytes = (const uint8_t *)&lod_bias;
    cprog.fbuf2_len = sizeof(lod_bias);
    cprog.fbuf2_offset = 0u;
    cprog.fbuf4_bytes = (const uint8_t *)cp_ring;
    cprog.fbuf4_len = sizeof(cp_ring);
    cprog.fbuf4_offset = 16u;
    assert(rw_g32_format_line(long_line, sizeof(long_line), &cprog) == 1);
    assert(strstr(long_line, "f=TimgA2Xhfn_IsrcN3Oc3mtc4nlnlnl") != NULL);
    assert(strstr(long_line, "fbuf2=len4+off0:-0.500") != NULL);
    assert(strstr(long_line, "fbuf4=len256+off16[(1.000,0.500,0.000,1.000),") != NULL);
    assert(strstr(long_line, ",(2.000,0.500,0.000,1.000)]") != NULL);

    /* OOB fbuf2 & fbuf4 offset guard */
    cprog.fbuf2_offset = 2u;
    cprog.fbuf4_offset = 252u;
    assert(rw_g32_format_line(long_line, sizeof(long_line), &cprog) == 1);
    assert(strstr(long_line, "fbuf2=len4+off2:oob") != NULL);
    assert(strstr(long_line, "fbuf4=len256+off252:oob") != NULL);
    unsetenv("ANNIBALE_G32_FILTER");
    printf("  [PASS] ANNIBALE_G32_FILTER + fbuf2 lod_bias + fbuf4[0..20] & OOB guard\n");

    printf("G32 probe host tests: PASS (6/6)\n");
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

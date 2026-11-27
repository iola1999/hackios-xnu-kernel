#!/usr/bin/env python3
"""Host ASan+UBSan test verifying blend_color plumbing in air_metal_render_draw.inc and encoder."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENCODER_SRC = ROOT / "userland-driver" / "annibalemetal_renderencoder.inc"
SUBMIT_CMDBUF_SRC = ROOT / "userland-driver" / "annibalemetal_commandbuffer.inc"
AIR_DRAW_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_render_draw.inc"
AIR_HDR_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_render.h"
METAL_M_SRC = ROOT / "userland-driver" / "annibalemetal.m"


def main() -> int:
    enc = ENCODER_SRC.read_text()
    cb = SUBMIT_CMDBUF_SRC.read_text()
    draw = AIR_DRAW_SRC.read_text()
    hdr = AIR_HDR_SRC.read_text()
    mm = METAL_M_SRC.read_text()

    assert "_blendColor[0] = r;" in enc and "_hasBlendColor = YES;" in enc, "encoder setter missing"
    assert "work->blendColor[i] = _blendColor[i];" in enc and "work->hasBlendColor = _hasBlendColor;" in enc, "recordDraw copy missing"
    assert ".blend_color = work->hasBlendColor ? work->blendColor : NULL," in cb, "commandbuffer rwa_draw init missing"
    assert "const float *blend_color;" in hdr and "const float *blend_color;" in mm, "rwa_draw ABI mismatch"
    assert "pipe->set_blend_color(pipe, &bc);" in draw, "pipe->set_blend_color call missing"

    # Extract the exact blend_color application block from air_metal_render_draw.inc and run under ASan+UBSan.
    start = draw.index("struct pipe_blend_color bc =")
    end = draw.index("pipe->set_blend_color(pipe, &bc);", start) + len("pipe->set_blend_color(pipe, &bc);")
    snippet = draw[start:end]

    harness = f"""
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

struct pipe_blend_color {{ float color[4]; }};
struct rwa_draw {{ const float *blend_color; }};
struct pipe_context;
struct pipe_context {{
    void (*set_blend_color)(struct pipe_context *, const struct pipe_blend_color *);
    float last[4];
    unsigned calls;
}};

static void mock_set_blend_color(struct pipe_context *p, const struct pipe_blend_color *bc) {{
    for (unsigned i = 0; i < 4; i++) p->last[i] = bc->color[i];
    p->calls++;
}}

static void apply(struct pipe_context *pipe, const struct rwa_draw *draw) {{
    {snippet}
}}

int main(void) {{
    struct pipe_context ctx = {{ .set_blend_color = mock_set_blend_color }};
    const float c[4] = {{ 0.25f, 0.5f, 0.75f, 0.5f }};
    struct rwa_draw d1 = {{ .blend_color = c }};
    apply(&ctx, &d1);
    assert(ctx.calls == 1);
    assert(fabsf(ctx.last[0] - 0.25f) < 1e-6f && fabsf(ctx.last[1] - 0.5f) < 1e-6f &&
           fabsf(ctx.last[2] - 0.75f) < 1e-6f && fabsf(ctx.last[3] - 0.5f) < 1e-6f);

    /* Subsequent draw without setBlendColor must reset Gallium blend color to zero. */
    struct rwa_draw d2 = {{ .blend_color = NULL }};
    apply(&ctx, &d2);
    assert(ctx.calls == 2);
    assert(ctx.last[0] == 0.0f && ctx.last[1] == 0.0f &&
           ctx.last[2] == 0.0f && ctx.last[3] == 0.0f);
    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="metal-blend-color-") as tmp:
        src = pathlib.Path(tmp) / "test.c"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(harness)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
    print("test-metal-blend-color: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

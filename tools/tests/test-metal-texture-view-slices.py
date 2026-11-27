#!/usr/bin/env python3
"""Host ASan+UBSan test for rwa_texture_set_view_slices (1D/2D/Cube array <-> non-array views)."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
AIR_TEX_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_texture.inc"
AIR_TEXELS_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_texels.inc"
DRV_TEX_SRC = ROOT / "userland-driver" / "annibalemetal_texture.inc"
DRV_MESA_SRC = ROOT / "userland-driver" / "annibalemetal_mesa.inc"


def extract_view_slice_helpers() -> str:
    text = AIR_TEX_SRC.read_text()
    start = text.index("static enum pipe_texture_target\npipe_target_from_rwa_type(")
    end = text.index("\nstatic bool\nrwa_try_retain_u(", start)
    texels = AIR_TEXELS_SRC.read_text()
    tstart = texels.index("static bool\ntexture_box(")
    tend = texels.index("\n/* How wide a row", tstart)
    return text[start:end] + "\n" + texels[tstart:tend]


HARNESS = r"""
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define u_minify(v, l) (((v) >> (l)) ? ((v) >> (l)) : 1u)

enum {
    RWA_TYPE_1D = 0,
    RWA_TYPE_1D_ARRAY = 1,
    RWA_TYPE_2D = 2,
    RWA_TYPE_2D_ARRAY = 3,
    RWA_TYPE_2D_MULTISAMPLE = 4,
    RWA_TYPE_CUBE = 5,
    RWA_TYPE_CUBE_ARRAY = 6,
    RWA_TYPE_3D = 7,
};

enum pipe_texture_target {
    PIPE_TEXTURE_1D = 1,
    PIPE_TEXTURE_2D = 2,
    PIPE_TEXTURE_3D = 3,
    PIPE_TEXTURE_CUBE = 4,
    PIPE_TEXTURE_1D_ARRAY = 6,
    PIPE_TEXTURE_2D_ARRAY = 7,
    PIPE_TEXTURE_CUBE_ARRAY = 8,
};

struct pipe_box { int x, y, z, width, height, depth; };
struct pipe_resource {
    enum pipe_texture_target target;
    unsigned width0, height0, depth0, last_level, array_size;
};
struct pipe_sampler_view { int dummy; };
struct rwa_device { int dummy; };
struct rwa_texture_shape { unsigned type, array_length; };
struct rwa_texture {
    struct rwa_device *device;
    struct pipe_resource *res;
    struct pipe_sampler_view *view;
    struct rwa_texture_shape shape;
    unsigned layers;
    unsigned first_layer;
};

static pthread_mutex_t rwa_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_view_freed, g_deferred, g_drained;

static void say(char *b, size_t n, const char *fmt, ...) { (void)b; (void)n; (void)fmt; }
static void write_zero_region(const struct rwa_texture *t, unsigned a, unsigned b, unsigned c)
{
    (void)t; (void)a; (void)b; (void)c;
}

static void rwa_deferred_drain_locked(struct rwa_device *d) { (void)d; g_drained++; }
static void rwa_deferred_defer_texture(struct rwa_device *d, struct pipe_resource *r,
                                       struct pipe_sampler_view *v)
{
    (void)d; (void)r; (void)v; g_deferred++;
}
static void pipe_sampler_view_reference(struct pipe_sampler_view **dst,
                                        struct pipe_sampler_view *src)
{
    if (*dst && !src) g_view_freed++;
    *dst = src;
}

__HELPERS__

int main(void)
{
    struct rwa_device dev = { 0 };
    struct pipe_sampler_view sv = { 1 };

    /* 1. 1DArray (4 slices) -> 1D view at slice 2 (Weather fragment_skybox_texture1d). */
    struct pipe_resource r1d = { .array_size = 4 };
    struct rwa_texture t1 = {
        .device = &dev, .res = &r1d, .view = &sv,
        .shape = { .type = RWA_TYPE_1D_ARRAY, .array_length = 4 },
        .layers = 4, .first_layer = 0,
    };
    rwa_texture_set_view_slices(&t1, RWA_TYPE_1D, 2, 1);
    assert(t1.shape.type == RWA_TYPE_1D && t1.first_layer == 2 && t1.layers == 1 && t1.shape.array_length == 1);
    assert(pipe_target_from_rwa_type(t1.shape.type, PIPE_TEXTURE_1D_ARRAY) == PIPE_TEXTURE_1D);
    assert(t1.view == NULL && g_view_freed == 1 && g_drained == 1);

    /* 2. 2DArray (8 slices) -> 2DArray subrange [2..4], then chained -> 2D at relative slice 1 (abs 3). */
    struct pipe_resource r2d = { .array_size = 8 };
    struct rwa_texture t2 = {
        .device = &dev, .res = &r2d,
        .shape = { .type = RWA_TYPE_2D_ARRAY, .array_length = 8 },
        .layers = 8, .first_layer = 0,
    };
    rwa_texture_set_view_slices(&t2, RWA_TYPE_2D_ARRAY, 2, 3);
    assert(t2.shape.type == RWA_TYPE_2D_ARRAY && t2.first_layer == 2 && t2.layers == 3 && t2.shape.array_length == 3);
    rwa_texture_set_view_slices(&t2, RWA_TYPE_2D, 1, 1);
    assert(t2.shape.type == RWA_TYPE_2D && t2.first_layer == 3 && t2.layers == 1 && t2.shape.array_length == 1);
    assert(pipe_target_from_rwa_type(t2.shape.type, PIPE_TEXTURE_2D_ARRAY) == PIPE_TEXTURE_2D);

    /* 3. CubeArray (18 faces = 3 cubes) -> Cube at face 6 (cube 1) & CubeArray of 2 cubes (12 faces). */
    struct pipe_resource rcube = { .array_size = 18 };
    struct rwa_texture t3 = {
        .device = &dev, .res = &rcube,
        .shape = { .type = RWA_TYPE_CUBE_ARRAY, .array_length = 3 },
        .layers = 18, .first_layer = 0,
    };
    rwa_texture_set_view_slices(&t3, RWA_TYPE_CUBE, 6, 6);
    assert(t3.shape.type == RWA_TYPE_CUBE && t3.first_layer == 6 && t3.layers == 6 && t3.shape.array_length == 1);
    assert(pipe_target_from_rwa_type(t3.shape.type, PIPE_TEXTURE_CUBE_ARRAY) == PIPE_TEXTURE_CUBE);

    struct rwa_texture t4 = {
        .device = &dev, .res = &rcube,
        .shape = { .type = RWA_TYPE_CUBE_ARRAY, .array_length = 3 },
        .layers = 18, .first_layer = 0,
    };
    rwa_texture_set_view_slices(&t4, RWA_TYPE_CUBE_ARRAY, 6, 12);
    assert(t4.shape.type == RWA_TYPE_CUBE_ARRAY && t4.first_layer == 6 && t4.layers == 12 && t4.shape.array_length == 2);
    assert(pipe_target_from_rwa_type(t4.shape.type, PIPE_TEXTURE_CUBE) == PIPE_TEXTURE_CUBE_ARRAY);

    /* 4. Out-of-bounds first_slice must be ignored without mutation. */
    rwa_texture_set_view_slices(&t4, RWA_TYPE_CUBE, 99, 6);
    assert(t4.shape.type == RWA_TYPE_CUBE_ARRAY && t4.first_layer == 6 && t4.layers == 12);

    /* 5. texture_box: 1DArray slices go into box.z (Gallium/freedreno convention) with box.y=0,
     * and slice views offset layer by texture->first_layer. */
    char why[128] = { 0 };
    struct pipe_box box = { 0 };
    r1d.target = PIPE_TEXTURE_1D_ARRAY;
    r1d.width0 = 256;
    r1d.height0 = 1;
    r1d.depth0 = 1;
    struct rwa_texture t1_arr = {
        .device = &dev, .res = &r1d,
        .shape = { .type = RWA_TYPE_1D_ARRAY, .array_length = 4 },
        .layers = 4, .first_layer = 0,
    };
    assert(texture_box(&t1_arr, 0, 3, NULL, NULL, &box, why, sizeof(why)));
    assert(box.x == 0 && box.y == 0 && box.z == 3 && box.width == 256 && box.height == 1 && box.depth == 1);
    assert(texture_box(&t1, 0, 0, NULL, NULL, &box, why, sizeof(why)));
    assert(box.y == 0 && box.z == 2 && box.width == 256 && box.height == 1 && box.depth == 1);

    r2d.target = PIPE_TEXTURE_2D_ARRAY;
    r2d.width0 = 16;
    r2d.height0 = 16;
    r2d.depth0 = 1;
    assert(texture_box(&t2, 0, 0, NULL, NULL, &box, why, sizeof(why)));
    assert(box.y == 0 && box.z == 3 && box.width == 16 && box.height == 16 && box.depth == 1);
    return 0;
}
"""


def main() -> int:
    drv_tex = DRV_TEX_SRC.read_text()
    drv_mesa = DRV_MESA_SRC.read_text()
    assert "texture_set_view_slices" in drv_tex, "driver texture view missing rwa_texture_set_view_slices call"
    assert 'dlsym(mesa.handle, "rwa_texture_set_view_slices")' in drv_mesa, "mesa dlsym missing"
    assert "MTLTextureType1D && _shape.type == MTLTextureType1DArray" in drv_tex, "1D<->1DArray missing"

    code = HARNESS.replace("__HELPERS__", extract_view_slice_helpers())
    with tempfile.TemporaryDirectory(prefix="metal-texview-slices-") as tmp:
        src = pathlib.Path(tmp) / "test.c"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
    print("test-metal-texture-view-slices: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

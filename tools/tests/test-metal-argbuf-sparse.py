#!/usr/bin/env python3
"""Host ASan+UBSan test for sparse [[id(n)]] and inline constant argument buffer layout/encoder."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARGENC_SRC = ROOT / "userland-driver" / "annibalemetal_compute_argencoder.inc"
DEV_CAPS_SRC = ROOT / "userland-driver" / "annibalemetal_device_caps.inc"
METAL_M_SRC = ROOT / "userland-driver" / "annibalemetal.m"
AIR_METAL_H = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal.h"
REFLECT_SRC = ROOT.parents[1] / "hackios-xnu-userland" / "tools" / "mesa" / "air" / "air_metal_reflect_vertex.inc"


def extract_flatten_helpers() -> str:
    text = DEV_CAPS_SRC.read_text()
    start = text.index("static unsigned\nrw_mtl_data_type_size(")
    end = text.index("\n- (id)newArgumentEncoderWithBufferBinding:", start)
    return text[start:end]


def extract_ready_method() -> str:
    text = ARGENC_SRC.read_text()
    start = text.index("- (BOOL)annibaleReady:(NSUInteger)index for:(const char *)what")
    end = text.index("\n/*\n * The three that record.", start)
    return text[start:end]


def extract_constant_data_method() -> str:
    text = ARGENC_SRC.read_text()
    start = text.index("- (void *)constantDataAtIndex:(NSUInteger)index")
    end = text.index("\n/*\n * A SET INSIDE A SET", start)
    return text[start:end]


def extract_layout_loop() -> str:
    text = REFLECT_SRC.read_text()
    start = text.index("   for (unsigned k = 0; k < n; k++) {\n      struct rwa_argbuf_layout *l = &out[k];")
    end = text.index("\n   if (count)", start)
    return text[start:end]


def extract_struct_block(path: pathlib.Path, start_marker: str, end_marker: str) -> str:
    text = path.read_text()
    start = text.index(start_marker)
    end = text.index(end_marker, start) + len(end_marker)
    return text[start:end]


HARNESS = r"""
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RWA_MAX_ARGBUF_MEMBERS 64

__AIR_METAL_DECLS__

/* Renamed copy from annibalemetal.m to verify byte-for-byte ABI match across repos. */
__KERNEL_METAL_DECLS__

static char log_buf[4096];
static size_t log_len;

static void
rw_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0 && log_len + (size_t)n < sizeof(log_buf)) {
        log_len += (size_t)n;
    }
}

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (why && why_size) {
        vsnprintf(why, why_size, fmt, ap);
    }
    va_end(ap);
}

static void ralloc_free(void *p) { (void)p; }

static int
finalize_layouts(struct rwa_argbuf_layout *out, unsigned n, const char *name,
                 char *why, size_t why_size)
{
    void *mem_ctx = NULL;
__LAYOUT_LOOP__
    return 0;
}

#include "air_metal_reflection.h"
#include "annibalemetal_reflection.inc"

__FLATTEN_HELPERS__

@interface FakeBuffer : NSObject {
@public
    uint8_t *storage;
    NSUInteger len;
}
- (void *)contents;
- (NSUInteger)length;
@end

@implementation FakeBuffer
- (void *)contents { return storage; }
- (NSUInteger)length { return len; }
@end

@interface FakeArgEncoder : NSObject {
@public
    FakeBuffer *_destination;
    NSUInteger _offset;
    BOOL _complained;
    BOOL _fromModule;
    struct rwa_argbuf_layout _layout;
}
- (BOOL)annibaleReady:(NSUInteger)index for:(const char *)what;
- (void *)constantDataAtIndex:(NSUInteger)index;
@end

@implementation FakeArgEncoder
__READY_METHOD__
__CONST_DATA_METHOD__
@end

int
main(void)
{
    /* 0. Cross-repo ABI check: air_metal.h vs annibalemetal.m struct sizes and field offsets. */
    _Static_assert(sizeof(struct rwa_argbuf_member) == sizeof(struct k_rwa_argbuf_member),
                   "rwa_argbuf_member size mismatch");
    _Static_assert(sizeof(struct rwa_argbuf_layout) == sizeof(struct k_rwa_argbuf_layout),
                   "rwa_argbuf_layout size mismatch");
    _Static_assert(offsetof(struct rwa_argbuf_member, id) == offsetof(struct k_rwa_argbuf_member, id), "id");
    _Static_assert(offsetof(struct rwa_argbuf_member, kind) == offsetof(struct k_rwa_argbuf_member, kind), "kind");
    _Static_assert(offsetof(struct rwa_argbuf_member, index) == offsetof(struct k_rwa_argbuf_member, index), "index");
    _Static_assert(offsetof(struct rwa_argbuf_member, write) == offsetof(struct k_rwa_argbuf_member, write), "write");
    _Static_assert(offsetof(struct rwa_argbuf_member, offset) == offsetof(struct k_rwa_argbuf_member, offset), "offset");
    _Static_assert(offsetof(struct rwa_argbuf_member, size) == offsetof(struct k_rwa_argbuf_member, size), "size");
    _Static_assert(offsetof(struct rwa_argbuf_member, name) == offsetof(struct k_rwa_argbuf_member, name), "name");
    assert((int)RWA_ARGBUF_CONSTANT == 3);
    assert((int)K_RWA_ARGBUF_CONSTANT == 3);

    @autoreleasepool {
        char why[256] = { 0 };

        /* 1. Weather pass_constants_encoder: 3 members with sparse id 30 and encoded_length=0. */
        struct rwa_argbuf_layout l1 = {
            .metal_index = 0, .encoded_length = 0, .alignment = 8, .member_count = 3,
            .members = {
                { .id = 0, .kind = RWA_ARGBUF_BUFFER, .name = "frame" },
                { .id = 1, .kind = RWA_ARGBUF_BUFFER, .name = "camera" },
                { .id = 30, .kind = RWA_ARGBUF_TEXTURE, .name = "linearDepth" },
            },
        };
        assert(finalize_layouts(&l1, 1, "pass_constants_encoder", why, sizeof(why)) == 0);
        assert(l1.encoded_length == 8u * 31u);

        /* 2. Duplicate id must still be rejected. */
        struct rwa_argbuf_layout ldup = l1;
        ldup.members[1].id = 30;
        assert(finalize_layouts(&ldup, 1, "dup_test", why, sizeof(why)) == -1);
        assert(strstr(why, "both id 30") != NULL);

        /* 3. Encoder validation: sparse id 30 accepted, id 31 & 64 rejected. */
        uint8_t raw[1024];
        memset(raw, 0, sizeof(raw));
        FakeBuffer *dest = [[FakeBuffer alloc] init];
        dest->storage = raw;
        dest->len = sizeof(raw);

        FakeArgEncoder *enc1 = [[FakeArgEncoder alloc] init];
        enc1->_destination = dest;
        enc1->_fromModule = YES;
        enc1->_layout = l1;
        assert([enc1 annibaleReady:0 for:"a buffer"] == YES);
        assert([enc1 annibaleReady:30 for:"a texture"] == YES);
        assert([enc1 annibaleReady:31 for:"a texture"] == NO);
        assert([enc1 annibaleReady:64 for:"a texture"] == NO);

        /* 4. Weather 10-member set with encoded_length=200 (25 slots) encoding ids 10..11, 20..24. */
        FakeArgEncoder *enc2 = [[FakeArgEncoder alloc] init];
        enc2->_destination = dest;
        enc2->_fromModule = YES;
        enc2->_layout.member_count = 10;
        enc2->_layout.encoded_length = 200;
        for (unsigned i = 0; i < 10; i++) enc2->_layout.members[i].id = i;
        assert([enc2 annibaleReady:10 for:"a buffer"] == YES);
        assert([enc2 annibaleReady:11 for:"a buffer"] == YES);
        assert([enc2 annibaleReady:20 for:"a texture"] == YES);
        assert([enc2 annibaleReady:24 for:"a texture"] == YES);
        assert([enc2 annibaleReady:25 for:"a texture"] == NO);

        /* 5. VFX mixed inline constant + descriptor layout: id=0 (352 B), non-zero ids (16/32/48 B), tex/samp at 26/27. */
        struct rwa_argbuf_layout lvfx = {
            .metal_index = 0, .encoded_length = 0, .alignment = 16, .member_count = 6,
            .members = {
                { .id = 0, .kind = RWA_ARGBUF_CONSTANT, .index = ~0u, .offset = 0, .size = 352, .name = "material" },
                { .id = 1, .kind = RWA_ARGBUF_CONSTANT, .index = ~0u, .offset = 352, .size = 16, .name = "tint" },
                { .id = 2, .kind = RWA_ARGBUF_CONSTANT, .index = ~0u, .offset = 368, .size = 32, .name = "uvXform" },
                { .id = 5, .kind = RWA_ARGBUF_CONSTANT, .index = ~0u, .offset = 400, .size = 48, .name = "bounds" },
                { .id = 26, .kind = RWA_ARGBUF_TEXTURE, .index = 0, .name = "tex0" },
                { .id = 27, .kind = RWA_ARGBUF_SAMPLER, .index = 0, .name = "samp0" },
            },
        };
        assert(finalize_layouts(&lvfx, 1, "vfx_material", why, sizeof(why)) == 0);
        assert(lvfx.encoded_length == 448u);

        FakeArgEncoder *enc3 = [[FakeArgEncoder alloc] init];
        enc3->_destination = dest;
        enc3->_offset = 64;
        enc3->_fromModule = YES;
        enc3->_layout = lvfx;

        void *p0 = [enc3 constantDataAtIndex:0];
        void *p1 = [enc3 constantDataAtIndex:1];
        void *p2 = [enc3 constantDataAtIndex:2];
        void *p5 = [enc3 constantDataAtIndex:5];
        assert(p0 == raw + 64 + 0);
        assert(p1 == raw + 64 + 352);
        assert(p2 == raw + 64 + 368);
        assert(p5 == raw + 64 + 400);

        /* Writes to each constant member must not overlap adjacent members. */
        memset(p0, 0x11, 352);
        memset(p1, 0x22, 16);
        memset(p2, 0x33, 32);
        memset(p5, 0x55, 48);
        assert(raw[64 + 0] == 0x11 && raw[64 + 351] == 0x11);
        assert(raw[64 + 352] == 0x22 && raw[64 + 367] == 0x22);
        assert(raw[64 + 368] == 0x33 && raw[64 + 399] == 0x33);
        assert(raw[64 + 400] == 0x55 && raw[64 + 447] == 0x55);

        /* Descriptor member id=26 and unknown id=3 must be rejected by constantDataAtIndex:. */
        log_len = 0;
        log_buf[0] = '\0';
        assert([enc3 constantDataAtIndex:26] == NULL);
        assert(strstr(log_buf, "names no inline constant member") != NULL);
        assert([enc3 constantDataAtIndex:3] == NULL);

        /* OOB check: offset + member.offset + member.size > dest length must return NULL. */
        enc3->_offset = 600; /* 600 + 400 + 48 = 1048 > 1024 */
        log_len = 0;
        log_buf[0] = '\0';
        assert([enc3 constantDataAtIndex:5] == NULL);
        assert(strstr(log_buf, "exceeds destination buffer length") != NULL);
        assert([enc3 constantDataAtIndex:0] == raw + 600); /* 600 + 352 = 952 <= 1024 */

        /* 6. VFX sparse [[id(n)]] > 63 (id=200,201,230,231,500,571) in a 176-byte argbuf. */
        struct rwa_argbuf_layout lsparse = {
            .metal_index = 2, .encoded_length = 176, .alignment = 16, .member_count = 6,
            .members = {
                { .id = 200, .kind = RWA_ARGBUF_TEXTURE, .index = 0, .name = "texA" },
                { .id = 201, .kind = RWA_ARGBUF_SAMPLER, .index = 0, .name = "sampA" },
                { .id = 230, .kind = RWA_ARGBUF_TEXTURE, .index = 1, .name = "texB" },
                { .id = 231, .kind = RWA_ARGBUF_SAMPLER, .index = 1, .name = "sampB" },
                { .id = 500, .kind = RWA_ARGBUF_BUFFER,  .index = 0, .name = "sceneBuf" },
                { .id = 571, .kind = RWA_ARGBUF_CONSTANT, .index = ~0u, .offset = 64, .size = 112, .name = "params" },
            },
        };
        assert(finalize_layouts(&lsparse, 1, "particle_quad_frag", why, sizeof(why)) == 0);
        assert(lsparse.encoded_length == 176u);

        FakeArgEncoder *enc4 = [[FakeArgEncoder alloc] init];
        enc4->_destination = dest;
        enc4->_offset = 0;
        enc4->_fromModule = NO;
        enc4->_layout = lsparse;
        assert([enc4 annibaleReady:200 for:"a texture"] == YES);
        assert([enc4 annibaleReady:201 for:"a sampler"] == YES);
        assert([enc4 annibaleReady:230 for:"a texture"] == YES);
        assert([enc4 annibaleReady:231 for:"a sampler"] == YES);
        assert([enc4 annibaleReady:500 for:"a buffer"] == YES);
        assert([enc4 annibaleReady:571 for:"constant data"] == YES);
        assert([enc4 annibaleReady:202 for:"a texture"] == NO);
        assert([enc4 constantDataAtIndex:571] == raw + 64);

        /* 7. newArgumentEncoderWithBufferBinding: flattening (nested texture2d_wrapper.storage + sparse ids). */
        const struct rwa_reflection_type f4 = { .data_type = 6, .data_size = 16, .alignment = 16, .name = "float4" };
        const struct rwa_reflection_type tex_leaf = {
            .data_type = 58, .data_size = 8, .alignment = 8, .name = "texture2d<float, sample>",
            .texture_type = 2, .texture_data_type = 3, .depth_texture = 0, .access = 0,
        };
        const struct rwa_reflection_type ptr_leaf = {
            .data_type = 60, .data_size = 8, .alignment = 8, .name = "device float*",
            .element = &f4, .access = 0, .constant_buffer = 0,
        };
        const struct rwa_reflection_member wrap_m[] = {
            { .name = "storage", .offset = 0, .indirect_argument_index = 230, .type = &tex_leaf },
        };
        const struct rwa_reflection_type wrap_st = {
            .data_type = 1, .data_size = 8, .alignment = 8, .name = "texture2d_wrapper",
            .member_count = 1, .members = wrap_m,
        };
        const struct rwa_reflection_member mat_m[] = {
            { .name = "color", .offset = 0, .indirect_argument_index = 0, .type = &f4 },
            { .name = "emissive", .offset = 16, .indirect_argument_index = 0, .type = &f4 },
        };
        const struct rwa_reflection_type mat_st = {
            .data_type = 1, .data_size = 32, .alignment = 16, .name = "MaterialConst",
            .member_count = 2, .members = mat_m,
        };
        const struct rwa_reflection_member top_m[] = {
            { .name = "mat", .offset = 0, .indirect_argument_index = 0, .type = &mat_st },
            { .name = "texWrap", .offset = 32, .indirect_argument_index = 230, .type = &wrap_st },
            { .name = "sceneBuf", .offset = 40, .indirect_argument_index = 500, .type = &ptr_leaf },
        };
        const struct rwa_reflection_type top_st = {
            .data_type = 1, .data_size = 48, .alignment = 16, .name = "VFXMaterial",
            .member_count = 3, .members = top_m, .is_argument_buffer = 1,
        };
        const struct rwa_reflection_argument rarg = {
            .name = "material", .type = 0, .index = 2, .active = 1, .array_length = 1,
            .buffer_type = &top_st, .constant_buffer = 1,
        };
        const struct rwa_compute_reflection refl_meta = { .argument_count = 1, .arguments = &rarg };
        const struct rwa_render_reflection rrefl_meta = { .vertex = &refl_meta, .fragment = NULL };
        NSString *rwhy = nil;
        MTLRenderPipelineReflection *rrefl = rw_render_reflection(&rrefl_meta, &rwhy);
        assert(rrefl != nil && [[rrefl vertexBindings] count] == 1);
        [rrefl release];
        MTLComputePipelineReflection *crefl = rw_compute_reflection(&refl_meta, &rwhy);
        assert(crefl != nil && [[crefl bindings] count] == 1);
        id<MTLBufferBinding> b = (id<MTLBufferBinding>)[crefl bindings][0];
        assert([b bufferPointerType].elementIsArgumentBuffer == YES);
        struct rwa_argbuf_layout flat = { 0 };
        unsigned maxEnd = 0;
        assert(rw_flatten_argbuf_struct([b bufferStructType], 0, &flat, &maxEnd, 0) == YES);
        assert(maxEnd == 48u && flat.member_count == 3);
        assert(flat.members[0].id == 0 && flat.members[0].kind == RWA_ARGBUF_CONSTANT &&
               flat.members[0].offset == 0 && flat.members[0].size == 32);
        assert(flat.members[1].id == 230 && flat.members[1].kind == RWA_ARGBUF_TEXTURE &&
               flat.members[1].offset == 32);
        assert(flat.members[2].id == 500 && flat.members[2].kind == RWA_ARGBUF_BUFFER &&
               flat.members[2].offset == 40);
        [crefl release];
        [enc4 release];

        [enc3 release];
        [enc2 release];
        [enc1 release];
        [dest release];
    }
    return 0;
}
"""


def main() -> int:
    air_decls = extract_struct_block(
        AIR_METAL_H,
        "enum rwa_argbuf_kind {",
        "struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];\n};",
    )
    kernel_decls = extract_struct_block(
        METAL_M_SRC,
        "enum rwa_argbuf_kind {",
        "struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];\n};",
    )
    kernel_decls = (
        kernel_decls
        .replace("enum rwa_argbuf_kind", "enum k_rwa_argbuf_kind")
        .replace("RWA_ARGBUF_", "K_RWA_ARGBUF_")
        .replace("struct rwa_argbuf_member", "struct k_rwa_argbuf_member")
        .replace("struct rwa_argbuf_layout", "struct k_rwa_argbuf_layout")
    )
    code = (
        HARNESS
        .replace("__AIR_METAL_DECLS__", air_decls)
        .replace("__KERNEL_METAL_DECLS__", kernel_decls)
        .replace("__LAYOUT_LOOP__", extract_layout_loop())
        .replace("__FLATTEN_HELPERS__", extract_flatten_helpers())
        .replace("__READY_METHOD__", extract_ready_method())
        .replace("__CONST_DATA_METHOD__", extract_constant_data_method())
    )
    with tempfile.TemporaryDirectory(prefix="metal-argbuf-sparse-") as tmp:
        src = pathlib.Path(tmp) / "test.m"
        exe = pathlib.Path(tmp) / "test"
        src.write_text(code)
        subprocess.run([
            "clang", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
            "-fsanitize=address,undefined", "-fno-objc-arc",
            f"-I{ROOT / 'userland-driver'}",
            "-framework", "Foundation", "-framework", "Metal", str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)
    print("test-metal-argbuf-sparse: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

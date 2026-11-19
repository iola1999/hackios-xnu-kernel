#!/usr/bin/env python3
"""Host ASan+UBSan unit test for MiniM2Scaler rotated biplanar YUV plane paths and abort."""

import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "xnu/files/iokit/Kernel/MiniM2Scaler.cpp"


def extract_m2_helpers(text: str) -> str:
    m_flags = re.search(r"(enum\s*\{\s*kM2FlagFlipH.*?\};)", text, re.S)
    if not m_flags:
        raise RuntimeError("Could not locate kM2FlagFlipH/kM2FlagRotate90 enum in MiniM2Scaler.cpp")
    m_aborted = re.search(
        r"(static inline bool\s+m2Aborted\b.*?^}\s*)", text, re.S | re.M
    )
    if not m_aborted:
        raise RuntimeError("Could not locate m2Aborted in MiniM2Scaler.cpp")
    m_scale = re.search(
        r"(/\* Fast integer bilinear/nearest.*?^static bool\s+m2ScalePlane\b.*?^}\s*)",
        text,
        re.S | re.M,
    )
    if not m_scale:
        raise RuntimeError("Could not locate m2ScalePlane in MiniM2Scaler.cpp")
    return m_flags.group(1) + "\n\n" + m_aborted.group(1) + "\n" + m_scale.group(1)


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define bcopy(src, dst, len) std::memmove((dst), (src), (len))

__M2_HELPERS__

static void test_xform_1to1(uint32_t channels, bool wide, uint32_t xform) {
    const uint32_t srcW = 8, srcH = 6;
    const bool rot90 = (xform & kM2FlagRotate90) != 0;
    const bool flipH = (xform & kM2FlagFlipH) != 0;
    const bool flipV = (xform & kM2FlagFlipV) != 0;
    const uint32_t dstW = rot90 ? srcH : srcW;
    const uint32_t dstH = rot90 ? srcW : srcH;
    const uint32_t elem = channels * (wide ? 2u : 1u);
    const uint32_t srcBPR = srcW * elem + 16u;
    const uint32_t dstBPR = dstW * elem + 16u;

    std::vector<uint8_t> src(srcH * srcBPR, 0);
    std::vector<uint8_t> dst(dstH * dstBPR, 0xaa);

    for (uint32_t y = 0; y < srcH; y++) {
        for (uint32_t x = 0; x < srcW; x++) {
            for (uint32_t c = 0; c < channels; c++) {
                uint32_t v = (y * 17u + x * 3u + c * 53u) & 0xffu;
                if (wide) {
                    reinterpret_cast<uint16_t *>(src.data() + y * srcBPR)[x * channels + c] =
                        static_cast<uint16_t>((v << 8) | (v ^ 0x5au));
                } else {
                    src[y * srcBPR + x * channels + c] = static_cast<uint8_t>(v);
                }
            }
        }
    }

    volatile uint32_t abortGen = 7u;
    bool ok = m2ScalePlane(src.data(), srcW, srcH, srcBPR, wide,
        dst.data(), dstW, dstH, dstBPR, wide, channels,
        0, 0, static_cast<uint64_t>(srcW) << 16, static_cast<uint64_t>(srcH) << 16,
        0, 0, dstW, dstH, false, xform, &abortGen, 7u);
    assert(ok);

    for (uint32_t y = 0; y < dstH; y++) {
        for (uint32_t x = 0; x < dstW; x++) {
            uint32_t ux = flipH ? (dstW - 1 - x) : x;
            uint32_t uy = flipV ? (dstH - 1 - y) : y;
            uint32_t sx = rot90 ? uy : ux;
            uint32_t sy = rot90 ? (dstW - 1 - ux) : uy;
            for (uint32_t c = 0; c < channels; c++) {
                if (wide) {
                    uint16_t got = reinterpret_cast<const uint16_t *>(dst.data() + y * dstBPR)[x * channels + c];
                    uint16_t exp = reinterpret_cast<const uint16_t *>(src.data() + sy * srcBPR)[sx * channels + c];
                    assert(got == exp);
                } else {
                    uint8_t got = dst[y * dstBPR + x * channels + c];
                    uint8_t exp = src[sy * srcBPR + sx * channels + c];
                    assert(got == exp);
                }
            }
        }
    }

    /* Explicit VideoToolbox SymmetricTransform orientation checks (no left/right mirror on CW90). */
    if (channels == 1 && !wide) {
        if (xform == kM2FlagRotate90) {
            assert(dst[0 * dstBPR + (dstW - 1)] == src[0 * srcBPR + 0]);
            assert(dst[0 * dstBPR + 0] == src[(srcH - 1) * srcBPR + 0]);
            assert(dst[(dstH - 1) * dstBPR + (dstW - 1)] == src[0 * srcBPR + (srcW - 1)]);
        } else if (xform == (kM2FlagRotate90 | kM2FlagFlipH | kM2FlagFlipV)) {
            assert(dst[0 * dstBPR + 0] == src[0 * srcBPR + (srcW - 1)]);
            assert(dst[(dstH - 1) * dstBPR + 0] == src[0 * srcBPR + 0]);
        }
    }
}

static void test_xform_scaled_and_abort() {
    const uint32_t srcW = 12, srcH = 8, dstW = 4, dstH = 6;
    std::vector<uint8_t> src(srcH * srcW * 2u, 0);
    std::vector<uint8_t> dst(dstH * dstW * 2u, 0);
    for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);

    volatile uint32_t abortGen = 3u;
    bool ok = m2ScalePlane(src.data(), srcW, srcH, srcW * 2u, false,
        dst.data(), dstW, dstH, dstW * 2u, false, 2u,
        0, 0, static_cast<uint64_t>(srcW) << 16, static_cast<uint64_t>(srcH) << 16,
        0, 0, dstW, dstH, true, kM2FlagRotate90 | kM2FlagFlipH, &abortGen, 3u);
    assert(ok);

    /* Abort generation mismatch must short-circuit before writing any pixels. */
    std::fill(dst.begin(), dst.end(), 0x55);
    abortGen = 4u;
    bool aborted = m2ScalePlane(src.data(), srcW, srcH, srcW * 2u, false,
        dst.data(), dstW, dstH, dstW * 2u, false, 2u,
        0, 0, static_cast<uint64_t>(srcW) << 16, static_cast<uint64_t>(srcH) << 16,
        0, 0, dstW, dstH, false, kM2FlagRotate90, &abortGen, 3u);
    assert(!aborted);
    for (uint8_t b : dst) assert(b == 0x55);
}

int main() {
    for (uint32_t xform = 0; xform < 8; xform++) {
        test_xform_1to1(1, false, xform);
        test_xform_1to1(2, false, xform);
        test_xform_1to1(1, true, xform);
        test_xform_1to1(2, true, xform);
    }
    test_xform_scaled_and_abort();
    std::puts("33 MiniM2Scaler rotated biplanar YUV & abort scenarios passed");
    return 0;
}
"""


def main() -> int:
    text = SRC.read_text()
    helpers = extract_m2_helpers(text)
    if "kM2MethodAbortTransfers" not in text or "fAbortGen" not in text:
        raise RuntimeError("Missing fAbortGen abort handling in MiniM2Scaler.cpp")
    code = HARNESS.replace("__M2_HELPERS__", helpers)
    with tempfile.TemporaryDirectory() as td:
        cpp = pathlib.Path(td) / "test_m2.cpp"
        exe = pathlib.Path(td) / "test_m2"
        cpp.write_text(code)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                str(cpp),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compile selector 74 and verify initialized outputs and caller buffer bounds."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "xnu/files/iokit/Kernel/MiniFramebufferUserClient.inc"

PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

enum { kFBMethodSupportedFrameInfo = 74, kIOReturnSuccess = 0 };
struct Arguments {
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
    void *structureOutput;
    uint32_t structureOutputSize;
};
static int dispatch(Arguments *args) {
    uint64_t *out = args->scalarOutput;
    switch (kFBMethodSupportedFrameInfo) {
'''

SUFFIX = r'''
    default: return -1;
    }
}

int main() {
    constexpr uint64_t sentinel = 0xaaaaaaaaaaaaaaaaULL;
    unsigned scenarios = 0;
    for (uint32_t capacity : {0u, 1u, 2u, 4u}) {
        for (uint32_t bytes : {0u, 1u, 63u, 64u, 65u}) {
            for (bool null_structure : {false, true}) {
                std::vector<uint64_t> scalars(capacity + 2, sentinel);
                std::vector<uint8_t> structure(bytes + 2, 0x5a);
                Arguments args = {
                    capacity ? scalars.data() + 1 : nullptr, capacity,
                    null_structure ? nullptr : structure.data() + 1, bytes,
                };
                assert(dispatch(&args) == kIOReturnSuccess);
                uint32_t returned = capacity < 2 ? capacity : 2;
                assert(args.scalarOutputCount == returned);
                for (uint32_t i = 0; i < capacity + 2; ++i)
                    assert(scalars[i] == (i > 0 && i <= returned ? 0 : sentinel));
                assert(args.structureOutputSize == bytes);
                for (uint32_t i = 0; i < bytes + 2; ++i)
                    assert(structure[i] == (!null_structure && i > 0 && i <= bytes ? 0 : 0x5a));
                if (capacity == 2 && bytes == 64 && !null_structure) {
                    // 23G90 reads both slots without checking the returned count.
                    uint32_t count = (uint32_t)scalars[1];
                    uint32_t selected_index = (uint32_t)scalars[2];
                    assert(count == 0 && selected_index == 0);
                }
                scenarios++;
            }
        }
    }
    printf("frame info: %u output-capacity and structure-buffer scenarios passed\n", scenarios);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="extract the production branch from a Git revision")
    args = parser.parse_args()
    text = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{args.revision}:{SOURCE}"], text=True
    ) if args.revision else (ROOT / SOURCE).read_text()
    branch = text[text.index("\tcase kFBMethodSupportedFrameInfo:"):
                  text.index("\tcase kFBMethodGetBlock:")]
    with tempfile.TemporaryDirectory(prefix="frame-info-") as directory:
        source = Path(directory) / "frame-info.cc"
        binary = Path(directory) / "frame-info"
        source.write_text(PREFIX + branch + SUFFIX)
        subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

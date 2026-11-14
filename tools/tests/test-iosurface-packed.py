#!/usr/bin/env python3
"""Check native plane storage, packed Metal views and allocation bounds on macOS."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/userland-driver"
SOURCE = Path(__file__).with_name("iosurface-packed-native.m")


def main():
    with tempfile.TemporaryDirectory(prefix="iosurface-packed-") as directory:
        binary = Path(directory) / "native-packed"
        subprocess.run(["clang", "-fblocks", "-Wall", "-Wextra", "-Werror",
                        "-Wno-deprecated-declarations", "-Wno-multichar", "-O1",
                        "-fsanitize=address,undefined", "-I", str(DRIVER),
                        str(SOURCE), str(DRIVER / "annibaledrawable.m"),
                        "-framework", "Foundation", "-framework", "IOKit",
                        "-framework", "IOSurface", "-framework", "Metal",
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

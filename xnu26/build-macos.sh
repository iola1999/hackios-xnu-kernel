#!/bin/bash
# Build the xnu-12377 kernel for redwood on macOS with Xcode. Extra make args are passed through
# (exporthdrs: only the headers, which xnu26/deps-macos.sh --stage2 needs).
# The symbol sets for kexts are not made (KEXT_CREATE_SYMBOL_SET): no kext is ever loaded, and
# the exports of the closed parts of the kernel are missing from them.
# env: MACHINE (default BCM2837), KERNEL_CONFIG (default DEVELOPMENT), RC_DARWIN_KERNEL_VERSION (default
#      25.6.0, the Darwin version of iOS 26.6; Apple's build takes it from the SDK's System.kext)
source "$(dirname "${BASH_SOURCE[0]}")/env-macos.sh"
cd "$XNU26_SRC" || exit 1
SRC=$PWD
export RC_DARWIN_KERNEL_VERSION=${RC_DARWIN_KERNEL_VERSION:-25.6.0}

# Reproducible builds, part one: the version string.
# config/newvers.pl stamps version[] with the output of `date` and with `git describe --dirty` of the
# source tree, and that string sits inside __TEXT,__cstring. A different build minute or a different
# commit count changes its length, which shifts every string after it, so two builds of the same
# sources differ over a megabyte of image. newvers.pl takes KERNEL_BUILD_DATE and KERNEL_BUILD_OBJROOT
# from the environment for exactly this reason. The defaults below are the values the verified
# f055cc0a image carries; they are constants on purpose and must be changed by hand when the version
# needs to say something else. The object root half spells out the source directory name and the
# build variant, so a build from another directory or with another MACHINE/KERNEL_CONFIG needs its
# own value. `whoami` also lands in the string; an unattended build has to run as the same user.
export KERNEL_BUILD_DATE=${KERNEL_BUILD_DATE:-Mon Oct 26 12:50:44 CST 2026}
export KERNEL_BUILD_OBJROOT=${KERNEL_BUILD_OBJROOT:-xnu12377[xnu-12377.121.6-1-gaec10a9-dirty]/BUILD/obj/DEVELOPMENT_ARM64_BCM2837}

# Reproducible builds, part two: LC_UUID.
# ld-27037.1 assigns a random UUID on every link (three links of one input give three UUIDs) and has
# no -uuid option, so the load command is stamped to a fixed value after linking. The constant is the
# UUID of the f055cc0a image; keeping it makes the verified image reproducible instead of merely
# stable, and any other value would make the same sources produce a different hash.
export KERNEL_UUID=${KERNEL_UUID:-c71f02b3-2965-3ecc-9398-8915830464c4}

${MAKE:-gmake} XCRUN="$XNU26_ROOT/xnu26/xcrun" SDKROOT="$XNU26_SDK" \
  ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=${KERNEL_CONFIG:-DEVELOPMENT} \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 LOGCOLORS=n \
  KEXT_CREATE_SYMBOL_SET=/usr/bin/true "$@" || exit $?

# exporthdrs and similar targets produce no kernel; stamp only when there is one.
kernel="$SRC/BUILD/obj/${KERNEL_CONFIG:-DEVELOPMENT}_ARM64_${MACHINE:-BCM2837}/mach.$(echo "${KERNEL_CONFIG:-DEVELOPMENT}" | tr 'A-Z' 'a-z').$(echo "${MACHINE:-BCM2837}" | tr 'A-Z' 'a-z')"
if [ -f "$kernel" ]; then
  python3 - "$kernel" "$KERNEL_UUID" <<'PY' || exit $?
import struct, sys, uuid
path, want = sys.argv[1], uuid.UUID(sys.argv[2]).bytes
image = bytearray(open(path, "rb").read())
if struct.unpack_from("<I", image, 0)[0] != 0xFEEDFACF:
    sys.exit("%s: not a thin 64-bit Mach-O, cannot stamp LC_UUID" % path)
offset, ncmds = 32, struct.unpack_from("<I", image, 16)[0]
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", image, offset)
    if size < 8:
        sys.exit("%s: malformed load command" % path)
    if cmd == 0x1B:                       # LC_UUID
        image[offset + 8:offset + 24] = want
        open(path, "wb").write(bytes(image))
        break
    offset += size
else:
    sys.exit("%s: no LC_UUID to stamp" % path)
PY
fi

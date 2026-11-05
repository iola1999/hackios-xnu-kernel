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

# The Wi-Fi driver's object (tools/mk-ath12k.sh) is linked into the kernel when it is there; make does not see it
# change, so the linked kernel is thrown away when it has (as xnu-build.sh does for ath11k's).
LX=$XNU26_ROOT/xnu-deps/build/ath12k/out/ath12k.o
if [ -f "$LX" ]; then now="$LX $(stat -f %m "$LX")"; else LX= now=; fi
if [ -d BUILD/obj ] && [ "$(cat BUILD/obj/redwood-objects 2>/dev/null)" != "$now" ]; then
  rm -f BUILD/obj/*/mach.*.unstripped BUILD/obj/*/mach.*.unstripped.noctf    # (the link's output, and its copy)
  echo "$now" > BUILD/obj/redwood-objects
fi

# Strip outer -j/--jobs flags: makedefs/MakeInc.cmd sets MAKEJOBS=--jobs=$(hw.ncpu) on sub-makes,
# and passing -jN to the outer make resets the jobserver and multiplies parallel clang processes.
make_args=()
while [ $# -gt 0 ]; do
  case "$1" in
  -j | --jobs) shift 2 ;;
  -j* | --jobs=*) shift ;;
  *) make_args+=("$1"); shift ;;
  esac
done

# Preserve incremental builds when conf/files* changes in an already-built tree:
# Every */conf/Makefile.template has `${OBJS}: $(firstword $(MAKEFILE_LIST))`, which makes all
# .o/.cpo files in a subsystem depend on the generated Makefile. If doconf updates Makefile with
# the current timestamp, every object in that subsystem recompiles; if Makefile is touched older
# than BUILD/obj/SETUP/config or DOCONFDEPS, make re-runs doconf across all subsystems.
# Run doconf for any subsystem whose conf/files* is newer than its generated Makefile, then place
# DOCONFDEPS at t_base and Makefile at t_base + 1 (strictly after SETUP/config and strictly before
# existing .o/.cpo files).
if [ -d "BUILD/obj/SETUP/config" ]; then
  python3 - "$SRC" "${MACHINE:-BCM2837}" "${KERNEL_CONFIG:-DEVELOPMENT}" <<'PY' || exit $?
import glob, os, subprocess, sys

src, machine, kconf = sys.argv[1], sys.argv[2], sys.argv[3]
arch_lc = "arm64"
mach_lc = machine.lower()
objroot = os.path.join(src, "BUILD/obj")
variant_dir = os.path.join(objroot, f"{kconf}_ARM64_{machine}")
setup_cfg = os.path.join(objroot, "SETUP/config")
doconf = os.path.join(src, "SETUP/config/doconf")
if not os.path.isdir(variant_dir) or not os.path.exists(doconf):
    sys.exit(0)

t_base = int(max(os.path.getmtime(setup_cfg), os.path.getmtime(doconf))) + 1
subsystems = ["osfmk", "bsd", "iokit", "libkern", "pexpert", "security", "san"]
master_dir = os.path.join(src, "config")
master_deps = [
    os.path.join(master_dir, f)
    for f in ("MASTER", f"MASTER.{arch_lc}", f"MASTER.{arch_lc}.{mach_lc}")
    if os.path.exists(os.path.join(master_dir, f))
]

for sub in subsystems:
    bdir = os.path.join(variant_dir, sub, kconf)
    mk = os.path.join(bdir, "Makefile")
    if not os.path.isfile(mk):
        continue
    for tmp in glob.glob(os.path.join(bdir, "*.tmp")):
        try:
            os.remove(tmp)
        except OSError:
            pass
    objs = glob.glob(os.path.join(bdir, "*.o")) + glob.glob(os.path.join(bdir, "*.cpo"))
    if not objs:
        continue
    conf_dir = os.path.join(src, sub, "conf")
    sub_deps = master_deps + [
        os.path.join(conf_dir, f)
        for f in ("Makefile.template", f"Makefile.{arch_lc}", "files", f"files.{arch_lc}")
        if os.path.exists(os.path.join(conf_dir, f))
    ]
    mk_mtime = os.path.getmtime(mk)
    if any(os.path.getmtime(d) > mk_mtime for d in sub_deps):
        env = os.environ.copy()
        env["SRCROOT"] = src
        env["OBJROOT"] = objroot
        env["SOURCE"] = conf_dir + "/"
        subprocess.check_call(
            [
                doconf, "-c", "-cpu", arch_lc, "-soc", mach_lc, "-platform", "BCM2837",
                "-d", bdir, "-s", conf_dir, "-m", master_dir, kconf,
            ],
            cwd=bdir,
            env=env,
        )
    for d in sub_deps:
        if os.path.getmtime(d) > t_base:
            os.utime(d, (t_base, t_base))
    t_mk = t_base + 1
    for fn in ("Makefile", "assym.s", "genassym.o"):
        p = os.path.join(bdir, fn)
        if os.path.isfile(p) and os.path.getmtime(p) > t_mk:
            os.utime(p, (t_mk, t_mk))
PY
fi

${MAKE:-gmake} XCRUN="$XNU26_ROOT/xnu26/xcrun" SDKROOT="$XNU26_SDK" REDWOOD_OBJECTS="$LX" \
  ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=${KERNEL_CONFIG:-DEVELOPMENT} \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 LOGCOLORS=n \
  KEXT_CREATE_SYMBOL_SET=/usr/bin/true "${make_args[@]}" || exit $?

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

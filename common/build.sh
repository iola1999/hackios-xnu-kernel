#!/bin/bash
# Build everything a device needs, in order, skipping what cannot be built on this machine.
#
# usage: common/build.sh <device> [userland/build.sh variables...]
#   common/build.sh qemu-virt
#   common/build.sh annibale GL=1
#
#   1. kernel   xnu26/apply.sh, xnu26/build-macos.sh
#   2. loader   xnuloader/build.sh                              -- only with clang + lld-link; a
#                                                                  profile may set LOADER_CFLAGS
#                                                                  (-DDIRECT_PHONE_PLAT for phones:
#                                                                  that build writes only
#                                                                  xnuloader/loader-phone.efi and
#                                                                  never touches loader.efi)
#   3. images   userland/build.sh (REST=1 and the profile's USERLAND_VARS)
#                                                               -- needs ios26/fetch.sh to have run
#   4. boot     mkxnuimg.py -> out/<device>-boot.img             -- phones: the UEFI image for `fastboot boot`
#
#   SKIP="kernel loader images boot"   leave steps out
. "$(dirname "$(readlink -f "$0")")/lib.sh"
load_device "$1"; shift
skip() { [[ " $SKIP " == *" $1 "* ]]; }
mkdir -p out

# which loader file the profile's flags produce (F04): the phone macro has its own output name,
# so the two variants never overwrite each other and every packaging step names what it packed.
LOADER_CFLAGS=${LOADER_CFLAGS:-}
LOADER_OUT=loader.efi
case " $LOADER_CFLAGS " in *" -DDIRECT_PHONE_PLAT "*) LOADER_OUT=loader-phone.efi ;; esac

if ! skip kernel; then
	if [ -x xnu26/build-macos.sh ] && command -v clang >/dev/null; then
		[ -d xnu-src/xnu-12377.121.6 ] || xnu26/apply.sh
		./xnu26/build-macos.sh || die "kernel build failed"
	else
		echo "build: xnu26/build-macos.sh not available: the kernel is not built"
	fi
fi

if ! skip loader; then
	if command -v clang >/dev/null && command -v lld-link >/dev/null; then
		EXTRA_CFLAGS="$LOADER_CFLAGS" xnuloader/build.sh || die "loader build failed: nothing gets packaged on top of the stale file"
		echo "build: loader xnuloader/$LOADER_OUT"
	else
		echo "build: no clang/lld-link: keeping the existing xnuloader/$LOADER_OUT (prebuilt only for the default variant)"
	fi
fi

if ! skip images; then
	have_userland
	[ -d ../hackios-xnu-userland/ios26/rootfs ] || die "no iOS files yet: run ios26/fetch.sh first"
	(cd ../hackios-xnu-userland/userland && env REST=1 $USERLAND_VARS "$@" ./build.sh) || die "userland/build.sh failed"
fi

if ! skip boot && [ "$KIND" = phone ]; then
	# the UEFI with the loader inside, the way `xl enter` makes it: the loader then brings up its USB link
	# The image carries the variant the profile asked for: wrong-platform loader bytes boot-loop
	# silently (in QEMU the kernel hangs at 616 bytes), so a missing one is refused out loud (F04).
	L=xnuloader/$LOADER_OUT
	[ -f $L ] || die "no xnuloader/$LOADER_OUT: run common/build.sh $DEVICE without SKIP=loader"
	python3 xnuloader/mkpayload.py -o out/usb-payload.bin loader.efi=$L --usb > /dev/null || die "mkpayload.py failed"
	FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE PAYLOAD_CAP=$PAYLOAD_CAP UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
		python3 mkxnuimg.py -p out/usb-payload.bin -o out/$DEVICE-boot.img || die "mkxnuimg.py failed"
	echo "build: out/$DEVICE-boot.img (loader $(basename $L)) -- boot it with: fastboot boot out/$DEVICE-boot.img (or common/run.sh $DEVICE)"
fi

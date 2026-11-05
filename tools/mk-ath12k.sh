#!/bin/bash
# Build the driver of the K90's Wi-Fi radio for the xnu-12377 kernel: ath12k, Qualcomm's driver for
# the WCN7850, as it is in Linux, unmodified, on the layer tools/mk-ath11k.sh builds ath11k on.
# The same three things meet here:
#
#   Linux's ath12k        drivers/net/wireless/ath/ath12k of Linux 7.0.14 (BSD-3-Clause-Clear),
#                         the files shared by its chips and those of its Wi-Fi 7 family (wifi7/);
#   FreeBSD's LinuxKPI    the headers of FreeBSD 15.1, patched with wifi/lx/patches/*.patch;
#   wifi/lx               ours: src/ as for ath11k, and ath12k/, which stands where ath12k's own
#                         pci.c and mhi.c would: the card on PCIe root complex 0, which the
#                         kernel's bsd/dev/mhi_pcie*.c power, boot and link to the driver's QMI.
#
# What is not built (pci.c, mhi.c, ahb.c, debugfs, ACPI, tracing, power management) is either
# done in wifi/lx/ath12k or not needed; a function of it that the built files still call becomes a
# stub that says its name and answers 0 (out/stubs.txt). The configuration tables of wifi7/mhi.c
# are data that wifi7/hw.c points to, and are built.
#
# What comes out, in xnu-deps/build/ath12k: src/ath12k/ (the driver's files, untouched),
# freebsd/sys/ (FreeBSD's, patched), out/ath12k.o (for the kernel), out/stubs.txt.
# The kernel (xnu26/build-macos.sh) links out/ath12k.o when it is there.
#
# usage: tools/mk-ath12k.sh [fetch]        "fetch" stops when the driver's sources are there
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/ath12k
ATH=drivers/net/wireless/ath
mkdir -p "$B"
cd "$B"

if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256" ]; then
	[ -f linux-$VERSION.tar.xz ] || curl -sL --fail -o linux-$VERSION.tar.xz \
		https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz
	echo "$SHA256  linux-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf src unpacked
	mkdir unpacked
	tar xJf linux-$VERSION.tar.xz -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath12k
	for f in unpacked/$ATH/ath12k/*.[ch] unpacked/$ATH/ath12k/wifi7/*.[ch]; do
		head -2 "$f" | grep -q "SPDX-License-Identifier: BSD-3-Clause-Clear" ||
			{ echo "mk-ath12k.sh: $f does not carry the licence expected" >&2; exit 1; }
	done
	mkdir src
	mv unpacked/$ATH/ath12k src/ath12k
	rm -rf unpacked
	echo "$VERSION $SHA256" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-ath12k.sh: ath12k of Linux $VERSION: $B/src/ath12k"; exit 0; }
W=$ROOT/wifi/lx

# --- FreeBSD's LinuxKPI and the kernel headers under it, with our patches on top (as mk-ath11k.sh)
FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$(cat "$W"/patches/*.patch 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	[ -f freebsd-$FREEBSD-src.txz ] || curl -sL --fail -o freebsd-$FREEBSD-src.txz \
		https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz
	echo "$FREEBSD_SHA256  freebsd-$FREEBSD-src.txz" | shasum -a 256 -c --quiet
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
S=$B/freebsd/sys

if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- compile
DRIVER="core hal wmi mac reg htc qmi dp dp_tx dp_rx dp_htt dp_peer ce peer dbring dp_mon fw p2p
	wifi7/core wifi7/wmi wifi7/mhi wifi7/ce wifi7/hw wifi7/hal_tx wifi7/hal_rx wifi7/dp_rx wifi7/dp_tx
	wifi7/dp wifi7/dp_mon wifi7/hal wifi7/hal_qcn9274 wifi7/hal_wcn7850 wifi7/hal_qcc2072"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
# xnu-deps/setup.sh links the cctools ld64 there on Linux; on macOS Xcode's ld is that linker
[ -e "$LD" ] || [ "$(uname -s)" != Darwin ] || LD=/usr/bin/ld
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w -ferror-limit=0
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
# the kernel's two headers come from this repository's copy of them (xnu/files, which both kernels share)
K=$ROOT/xnu/files/bsd/sys
DFLAGS="-Isrc/ath12k -Isrc -include $W/include/lx_prelude.h -include $W/ath12k/lx_ath12k.h -DCONFIG_ATH12K_DEBUG
	-DLX_POOL_MB=96u -DLX_RINGS_MB=64u -DLX_ALIGN_NATURAL
	-DKBUILD_MODNAME=\"ath12k\"
	-DLX_REDWOOD_LX_H=\"$K/redwood_lx.h\" -DLX_REDWOOD_QMI_H=\"$K/redwood_qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$ROOT/tools/mk-ath12k.sh" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
OBJECTS=
for f in $DRIVER; do
	o=obj/ath12k_$(echo $f | tr / _).o
	OBJECTS="$OBJECTS $o"
	compile src/ath12k/$f.c $o $DFLAGS &
done
for f in "$W"/src/*.c "$W"/ath12k/*.c; do
	[ "$(basename "$f")" = lx_bus.c ] && continue		# (ath11k's)
	o=obj/$(basename "$(dirname "$f")")_$(basename "$f" .c).o
	OBJECTS="$OBJECTS $o"
	compile "$f" $o $DFLAGS -I$W/src &
done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1

# --- one object, as mk-ath11k.sh ties ath11k's
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
nm -u -j out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|redwood_qmi_*|section\$*) continue ;;
	ath12k_*|__ath12k_*) ;;
	*) echo "mk-ath12k.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath12k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath12k.sh: out/ath12k.o: $(size out/ath12k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $(nm -u -j out/ath12k.o | grep -vc 'section\$') names"

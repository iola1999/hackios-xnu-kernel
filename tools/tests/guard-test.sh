#!/bin/sh
# The speakers' guard (bsd/dev/redwood/redwood_guard.c) on this machine: tools/tests/guard_test.c built against it and run.
# The kernel's header is all it needs, and an empty sys/ioccom.h beside it (the ioctl numbers are not used).
set -e
cd "$(dirname "$0")/../.."
SRC=xnu-src/xnu-8796.121.2/bsd
[ -f "$SRC/dev/redwood/redwood_guard.c" ] || SRC=xnu/files/bsd
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/sys"
cp $SRC/sys/redwood_audio.h "$tmp/sys/"
printf '#define _IOW(g, n, t) 0\n' > "$tmp/sys/ioccom.h"
cc -O2 -Wall -DKERNEL -I"$tmp" tools/tests/guard_test.c $SRC/dev/redwood/redwood_guard.c -lm -o "$tmp/guard_test"
"$tmp/guard_test"

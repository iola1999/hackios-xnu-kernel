#!/bin/bash
# Build and run the tests of the QMI helpers (bsd/dev/annibale_qmi.c of the kernel tree) on this
# machine, under the address and undefined-behaviour sanitizers.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/qmi-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/annibale_qmi.h" "$K/bsd/sys/annibale_qrtr.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/test" "$HERE/test_qmi.c" "$HERE/test_encode.c" "$HERE/test_router.c" "$K/bsd/dev/annibale/annibale_qmi.c"
"$B/test"

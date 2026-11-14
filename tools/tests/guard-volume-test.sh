#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
guard_bsd=${GUARD_BSD_SOURCE:-xnu/files/bsd}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
mkdir -p "$test_dir/sys"
cp "$guard_bsd/sys/annibale_audio.h" "$test_dir/sys/"
printf '#define _IOW(g, n, t) 0\n' > "$test_dir/sys/ioccom.h"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -DKERNEL -I"$test_dir" tools/tests/guard_volume_test.c "$guard_bsd/dev/annibale/annibale_guard.c" -lm -o "$test_dir/guard_volume_test"
"$test_dir/guard_volume_test" pcm
"$test_dir/guard_volume_test" hardware

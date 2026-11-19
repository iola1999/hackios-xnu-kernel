#!/bin/bash
# Build and run the tests of the QMI helpers (bsd/dev/annibale_qmi.c of the kernel tree) on this
# machine, under the address and undefined-behaviour sanitizers.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/qmi-test
rm -rf "$B"
mkdir -p "$B/inc/sys" "$B/qrtr-inc/kern" "$B/qrtr-inc/libkern" "$B/qrtr-inc/pexpert" "$B/qrtr-inc/sys"
ln -s "$K/bsd/sys/annibale_qmi.h" "$K/bsd/sys/annibale_qrtr.h" "$B/inc/sys/"
cat > "$B/qrtr-inc/kern/kalloc.h" <<'EOF'
#pragma once
#include <stdlib.h>
#define Z_WAITOK 0
#define Z_ZERO   1
static inline void *kalloc_data(size_t s, int f) { return (f & Z_ZERO) ? calloc(1, s) : malloc(s); }
static inline void kfree_data(void *p, size_t s) { (void)s; free(p); }
EOF
cat > "$B/qrtr-inc/kern/locks.h" <<'EOF'
#pragma once
#include <pthread.h>
typedef int lck_grp_t;
typedef pthread_mutex_t lck_mtx_t;
#define LCK_GRP_DECLARE(n, s) lck_grp_t n __attribute__((unused))
#define LCK_MTX_DECLARE(n, g) lck_mtx_t n = PTHREAD_MUTEX_INITIALIZER
static inline void lck_mtx_lock(lck_mtx_t *m) { pthread_mutex_lock(m); }
static inline void lck_mtx_unlock(lck_mtx_t *m) { pthread_mutex_unlock(m); }
EOF
cat > "$B/qrtr-inc/kern/thread.h" <<'EOF'
#pragma once
#include <pthread.h>
typedef pthread_t thread_t;
static inline thread_t current_thread(void) { return pthread_self(); }
EOF
echo '#include <string.h>' > "$B/qrtr-inc/libkern/libkern.h"
cat > "$B/qrtr-inc/pexpert/pexpert.h" <<'EOF'
#pragma once
static inline int PE_parse_boot_argn(const char *k, void *v, unsigned s) { (void)k; (void)v; (void)s; return 0; }
EOF
cat > "$B/qrtr-inc/sys/annibale_input.h" <<'EOF'
#pragma once
#include <stdint.h>
void annibale_wifi_say(const char *fmt, ...);
void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *data, uint32_t length));
EOF
echo '#pragma once' > "$B/qrtr-inc/sys/systm.h"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread"
for src in "$HERE/test_qmi.c" "$HERE/test_encode.c" "$HERE/test_router.c" "$K/bsd/dev/annibale/annibale_qmi.c"; do
	${CC:-cc} $FLAGS -I"$B/inc" -c "$src" -o "$B/$(basename "$src" .c).o"
done
${CC:-cc} $FLAGS -I"$B/qrtr-inc" -I"$B/inc" -c "$K/bsd/dev/annibale/annibale_qrtr.c" -o "$B/annibale_qrtr.o"
${CC:-cc} $FLAGS -o "$B/test" "$B"/*.o
"$B/test"

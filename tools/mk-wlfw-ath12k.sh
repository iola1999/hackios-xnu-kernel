#!/bin/bash
# Give the xnu-12377 kernel's build ath12k's descriptions of the messages the WCN7850's firmware speaks,
# for bsd/dev/mhi_pcie_wlfw.c (PCIe step 9, a stand-in for ath12k's own qmi.c). As tools/mk-wlfw.sh does for
# ath11k: the structures of qmi.h, the tables of qmi.c and this chip's copy engine tables (wifi7/hw.c names them,
# wifi7/ce.c holds them) are
# taken out as they are and written as one header into the kernel's build directory, where the compiler finds
# it and git does not. ath12k's text is BSD-3-Clause-Clear and is not kept in the repository; the six files
# are fetched from the stable tree at a pinned tag and checked against pinned digests.
# Without that header mhi_pcie_wlfw.c compiles to a line that says so.
#
# usage: XNU26_SRC=<xnu-12377 tree> tools/mk-wlfw-ath12k.sh      then build the kernel (xnu26/build-macos.sh)
set -e
TAG=v7.0.14
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
SRC=${XNU26_SRC:-$ROOT/xnu-src/xnu-12377.121.6}
B=$ROOT/xnu-deps/build/ath12k-qmi
OUT=$SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw-ath12k.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
mkdir -p "$B"
while read -r digest file; do
	local_name=$B/$(echo "$file" | tr / _)
	if [ "$(shasum -a 256 "$local_name" 2>/dev/null | cut -d' ' -f1)" != "$digest" ]; then
		curl -sL --fail -o "$local_name" \
			"https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/net/wireless/ath/ath12k/$file?h=$TAG"
		[ "$(shasum -a 256 "$local_name" | cut -d' ' -f1)" = "$digest" ] ||
			{ echo "mk-wlfw-ath12k.sh: $file at $TAG is not the file pinned here" >&2; exit 1; }
	fi
	head -2 "$local_name" | grep -q "SPDX-License-Identifier: BSD-3-Clause-Clear" ||
		{ echo "mk-wlfw-ath12k.sh: $file does not carry the licence expected" >&2; exit 1; }
done <<'PINS'
f338113110533a9f41bf3ffa8a44a6bb42dc8dbf36271144bf907cfd633a8db6 qmi.c
55879b7cab142c3f1be07f09fce7567f0ede616e92b5ec52a1d400844c2f8327 qmi.h
82326310931d2700b53871608f8b02ce610e47413e350818a6994729d6a1acba ce.h
03377943660bb0346a3baeb8f802cedf75e949175a61ea3270fe2878faff2e33 htc.h
ac2590222ab4e51d7553d41fcb669ab8991f89b35f83818a5146b1b5cb19bc7c wifi7/hw.c
71a17ab6ca84b1445d38ae8467d28e4bd96ca58d692002807c7a393ddc537d59 wifi7/ce.c
PINS
python3 - "$B" "$TAG" > "$OUT/redwood_wlfw_ath12k.h.new" <<'PY'
import re
import sys

src, tag = sys.argv[1], sys.argv[2]
def read(name):
    return open('%s/%s' % (src, name)).read()
c, h, ce, htc, hw, tables = read('qmi.c'), read('qmi.h'), read('ce.h'), read('htc.h'), read('wifi7_hw.c'), read('wifi7_ce.c')
print('/* ath12k\'s qmi.h, the message tables of its qmi.c and the WCN7850\'s copy engine tables (Linux %s,' % tag)
print(' * BSD-3-Clause-Clear), put together by tools/mk-wlfw-ath12k.sh: not to be edited, not to be kept. */')
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define __iomem
#define BIT(n) (1UL << (n))
struct list_head { struct list_head *next, *prev; };
struct work_struct { int unused; };
struct workqueue_struct;
typedef struct { int unused; } spinlock_t;
#define lockdep_assert_held(lock) ((void)(lock))
''')
print(re.sub(r'^#include <linux/[^>]*>\n', '', h, flags=re.M))
print(''.join(re.findall(r'^#define (?:SLEEP_CLOCK_SELECT_INTERNAL_BIT|HOST_CSTATE_BIT|PLATFORM_CAP_PCIE_GLOBAL_RESET)\s.*\n', c, re.M)))
print('\n'.join(re.findall(r'static const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S)))
print('/* as Linux has it on a little-endian machine, whatever this kernel\'s headers define */')
print('#undef __BIG_ENDIAN\n#define __cpu_to_le32(x) (x)')
print(''.join(re.findall(r'^#define CE_ATTR_\w+.*\n', ce, re.M)))
print(''.join(re.findall(r'^#define PIPEDIR_\w+.*\n', ce, re.M)))
for name in ('service_to_pipe', 'ce_pipe_config'):
    print(re.search(r'struct %s \{.*?\n\};\n' % name, ce, re.S).group(0))
print(re.search(r'enum ath12k_htc_svc_gid \{.*?\n\};\n', htc, re.S).group(0))
print(re.search(r'#define SVC\(group, idx\).*?\n.*?\n', htc).group(0))
print(re.search(r'enum ath12k_htc_svc_id \{.*?\n\};\n', htc, re.S).group(0))
print('#undef SVC')
entry = hw[hw.index('.name = "wcn7850 hw2.0"'):]
entry = entry[:entry.index('\n\t},')]
def of(field):
    return re.search(r'\.%s =\s*(\w+),' % field, entry).group(1)
for kind, field in (('ce_pipe_config', 'target_ce_config'), ('service_to_pipe', 'svc_to_ce_map')):
    print('static ' + re.search(r'const struct %s\s+%s\[\] = \{.*?\n\};\n' % (kind, of(field)), tables, re.S).group(0))
print('#define WLFW_PIPES\t\t%s\n#define WLFW_PIPE_COUNT\t\t%s' % (of('target_ce_config'), of('target_ce_count')))
print('#define WLFW_SERVICES\t\t%s\n#define WLFW_SERVICE_COUNT\t%s' % (of('svc_to_ce_map'), of('svc_to_ce_map_len')))
print('#define WLFW_SERVICE_INSTANCE\t%s' % of('qmi_service_ins_id'))
PY
cmp -s "$OUT/redwood_wlfw_ath12k.h.new" "$OUT/redwood_wlfw_ath12k.h" 2>/dev/null && rm "$OUT/redwood_wlfw_ath12k.h.new" ||
	mv "$OUT/redwood_wlfw_ath12k.h.new" "$OUT/redwood_wlfw_ath12k.h"
echo "mk-wlfw-ath12k.sh: $OUT/redwood_wlfw_ath12k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/redwood_wlfw_ath12k.h") tables)"

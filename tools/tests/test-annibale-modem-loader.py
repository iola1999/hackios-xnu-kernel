#!/usr/bin/env python3
"""Host tests for annibale IPA/MPSS PAS loader, modem=0 gate, and SMEM 421 sanitization."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ANNIBALE = ROOT / "xnu/files/bsd/dev/annibale"

ipa_src = (ANNIBALE / "annibale_ipa.c").read_text()
adsp_fw_src = (ANNIBALE / "annibale_adsp_fw.inc").read_text()
boot_inc = (ANNIBALE / "annibale_modem_boot.inc").read_text()
svc_inc = (ANNIBALE / "annibale_modem_svc.inc").read_text()
modem_src = (ANNIBALE / "annibale_modem.c").read_text()
qmi_src = (ANNIBALE / "annibale_qmi.c").read_text()
qrtr_src = (ANNIBALE / "annibale_qrtr.c").read_text()

# Static structural assertions directly against the kernel sources.
assert '#define IPA_WRAPPER_PHYS        0x03e00000ULL' in ipa_src
assert '#define IPA_GSI_PHYS            0x03e04000ULL' in ipa_src
assert '#define SCM_MP_ASSIGN           SCM_CALL(0xc, 0x16)' in ipa_src
assert '#define SCM_MP_ASSIGN_ARGINFO   0x1117ULL' in ipa_src
assert '#define QCOM_VMID_HLOS          3u' in ipa_src
assert '#define QCOM_PERM_RW            6u' in ipa_src
assert '#define IPA_TZ_HDR_SLOTS        3u' in ipa_src
assert 'x0 = 1ULL;' in ipa_src and 'x5 = a3;' in ipa_src
assert '(int64_t)rx0 == -12' in ipa_src
assert '(int64_t)rx0 == -12' in adsp_fw_src
assert 'kmem_alloc_contig(' in ipa_src
assert '#define MPSS_DSM1_MEM_PHYS              0x84a00000ULL' in modem_src
# QMI_LOC REG_EVENTS names its client: TLV 0x10 clientStrId "gps" and TLV 0x11 clientType 1 follow both masks.
assert modem_src.count("0x10, 0x04, 0x00, 'g', 'p', 's', 0, 0x11, 0x04, 0x00, 0x01, 0, 0, 0 };") == 2
assert '#define MPSS_DSM1_MEM_SIZE              0x04900000ULL' in modem_src
assert '#define MPSS_DSM2_MEM_PHYS              0x89300000ULL' in modem_src
assert '#define MPSS_DSM2_MEM_SIZE              0x00a80000ULL' in modem_src
assert '#define MPSS_VMID_MSA                   15u' in modem_src
assert '#define MPSS_DTB_PAS_ID                 38u' in modem_src
assert '#define MPSS_DTB_PAS_ID_ALT             0u' in modem_src
assert '#define SMP2P_MPSS_FATAL                (1u << 0)' in modem_src
assert '#define SMP2P_MPSS_HANDOVER             (1u << 2)' in modem_src
assert 'SMEM GLINK queues ready' in boot_inc
assert 'MODEM_STATE_OFF' in modem_src
assert 'mpss_glink_init_preboot();' in boot_inc
assert 'SMP2P_VERSION | (SMP2P_SSR_ACK << 8)' in boot_inc
assert 'mpss_glink_log_stall_diag(' in boot_inc
assert '#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)' in ipa_src
assert 'annibale_tz_pas_shutdown(' in ipa_src and 'annibale_tz_pas_shutdown(' in boot_inc
assert 'annibale_modem_stop(' in boot_inc and 'annibale_modem_return_quiesce(' in boot_inc
loader_ret_src = (ROOT / "xnu/files/osfmk/arm64/loader_return.c").read_text()
assert 'annibale_modem_stop();' in loader_ret_src and 'annibale_modem_return_quiesce();' in loader_ret_src
assert 'mpss_glink_log_stall_diag(tick)' in boot_inc and 'modem_worker_glink_tick(' in modem_src
assert 'SYSCTL_STRING(_kern_annibale, OID_AUTO, modem_loc_status, CTLFLAG_RD' in modem_src
assert 'modem_format_loc_status(' in svc_inc and 'modem_loc_diag' in svc_inc
smem_find_body = boot_inc[boot_inc.index("smem_find_mpss_item("):boot_inc.index("smem_alloc_mpss_item(")]
assert 'smem_find_item_in_partition(SMEM_HOST_GLOBAL' not in smem_find_body
assert 'mpss_glink_lock' in modem_src
assert 'GLINK_CMD_TX_SHORT_DATA' in modem_src
assert 'GLINK_CMD_TX_DATA_CONT' in modem_src
assert '"modemfirmware_a"' in (ANNIBALE / "annibale_ufs_rw.inc").read_text()
assert '"modemfirmware_b"' in (ANNIBALE / "annibale_ufs_rw.inc").read_text()
assign1_pos = boot_inc.index('annibale_tz_assign_mem(MPSS_DSM1_MEM_PHYS')
assign2_pos = boot_inc.index('annibale_tz_assign_mem(MPSS_DSM2_MEM_PHYS')
dtb_load_pos = boot_inc.index('annibale_load_mdt_image("modem_dtb"', assign1_pos)
assert assign1_pos < assign2_pos < dtb_load_pos
assert 'MPSS_VMID_MSA, false, &dsm1_res1' in boot_inc
assert 'MPSS_VMID_MSA, false, &dsm2_res1' in boot_inc
assert 'annibale-modem: HYP_ASSIGN(excl) dsm1=(%lld,%#llx) dsm2=(%lld,%#llx)' in boot_inc
assert 'annibale-modem: SMP2P slave-kernel val=%#x flags=%#x at %u ms' in boot_inc
assert 'SMP2P_MPSS_FATAL' in boot_inc
assert '#define MPSS_GLINK_MAX_INTENTS          64u' in modem_src
assert '#define QMI_SVC_LOC                     0x10u' in modem_src
assert '#define QMI_LOC_GET_SUPPORTED_FIELDS    0x001fu' in modem_src
assert '#define QMI_LOC_INFORM_CLIENT_REV       0x0020u' in modem_src
assert '#include "annibale_modem_svc.inc"' in modem_src
assert 'modem_start_local_services();' in modem_src
assert 'annibale_qrtr_publish(' in qrtr_src
assert 'qrtr_announce_local_to(header->from_node);' in qrtr_src
assert 'msm/modem/root_pd' in svc_inc and 'msm/modem/oem_pd' in svc_inc
assert 'annibale_ufs_part_read(' in svc_inc and 'annibale_ufs_var_io' not in svc_inc

# Extract production functions for ASan+UBSan execution:
# - annibale_tz_smc -> mocked
# - dc civac / dmb / dsb -> no-ops on host
ipa_fn_start = ipa_src.index("static int64_t\nannibale_tz_smc(")
ipa_loader_slice = ipa_src[ipa_fn_start:]
ipa_loader_slice, n_smc = re.subn(
    r"static int64_t\nannibale_tz_smc\(.*?\n\}\n",
    "static int64_t\nannibale_tz_smc(uint64_t fn, uint64_t arginfo, uint64_t a0, "
    "uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *res1_out)\n"
    "{ return host_tz_smc(fn, arginfo, a0, a1, a2, a3, res1_out); }\n",
    ipa_loader_slice, count=1, flags=re.S,
)
assert n_smc == 1
ipa_loader_slice = re.sub(
    r'__asm__ volatile\("dc civac, %0"[^;]*\);',
    'host_cacheclean(line);',
    ipa_loader_slice,
)
ipa_loader_slice = re.sub(
    r'__asm__ volatile\([^;]+dsb sy[^;]*\);',
    'host_dsb_count++;',
    ipa_loader_slice,
)
ipa_loader_slice = ipa_loader_slice.replace('__builtin_arm_dsb(0xf);', 'host_dsb_count++;')
# Forward-declare PE_parse_boot_argn before ipa_ensure_tz_hdr_pages since the C harness defines it below.
ipa_loader_slice = "static int PE_parse_boot_argn(const char *k, void *out, unsigned sz);\n" + ipa_loader_slice

sanitize_match = re.search(
    r"static void\nmodem_sanitize_crash_str\(.*?\n\}",
    boot_inc, flags=re.S,
)
assert sanitize_match is not None
sanitize_fn = sanitize_match.group()

init_match = re.search(
    r"void\nannibale_modem_init\(void\)\n\{.*?\n\}",
    modem_src, flags=re.S,
)
assert init_match is not None
init_fn = init_match.group()

# Extract GLINK state & SMEM/GLINK functions for ASan+UBSan protocol tests.
glink_state_match = re.search(
    r"struct mpss_glink_intent \{.*?\} mpss_glink = \{.*?\};",
    modem_src, flags=re.S,
)
assert glink_state_match is not None
glink_state_src = glink_state_match.group()

boot_glink_start = boot_inc.index("struct modem_smp2p_item {")
boot_glink_end = boot_inc.index("static void\nmodem_sanitize_crash_str(")
boot_glink_src = boot_inc[boot_glink_start:boot_glink_end].replace(
    '__asm__ volatile("dmb ishld" ::: "memory");', '',
)

stall_diag_start = boot_inc.index("static void\nmodem_log_crash_reason(")
stall_diag_end = boot_inc.index("static void\nmodem_bringup_sequence(")
stall_diag_src = boot_inc[stall_diag_start:stall_diag_end].replace(
    '__builtin_arm_dsb(0xf);', '',
)

modem_glink_start = modem_src.index("static int\nmpss_qrtr_send(")
modem_glink_end = modem_src.index("/* ---- QMI Client State Machine")
modem_glink_src = modem_src[modem_glink_start:modem_glink_end]

boot_tail_start = boot_inc.index("static void\nmodem_worker_glink_tick(")
boot_tail_src = boot_inc[boot_tail_start:boot_inc.index("#endif /* _ANNIBALE_MODEM_BOOT_INC_ */")]

qmi_codec_start = qmi_src.index("size_t\nannibale_qmi_encode_msg(")
qmi_codec_end = qmi_src.index("bool\nannibale_qmi_check_result(")
qmi_codec_src = qmi_src[qmi_codec_start:qmi_codec_end]

svc_host_src = re.sub(r'__asm__ volatile\("dc civac, %0"[^;]*\);', 'host_cacheclean(line);', svc_inc)
svc_host_src = svc_host_src.replace('__asm__ volatile("dmb ishld" ::: "memory");', '')
svc_host_src = svc_host_src.replace('__builtin_arm_dsb(0xf);', 'host_dsb_count++;')

harness = rf'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef uint32_t ppnum_t;
typedef void *vfs_context_t;
typedef void *vnode_t;
typedef void *caddr_t;
typedef void *thread_t;
struct vnode_attr {{ uint64_t va_data_size; }};
struct ipa_elf32_phdr {{ uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; }};

#define PAGE_SHIFT 14u
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1u)
#define NULLVP NULL
#define Z_WAITOK 0
#define Z_NOWAIT 0
#define Z_ZERO 1
#define KERN_SUCCESS 0
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define VM_WIMG_DEFAULT 1
#define VM_WIMG_WCOMB 2
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define VATTR_INIT(v) ((void)(v))
#define VATTR_WANTED(v, f) ((void)(v))
#define DEVFS_CHAR 0
#define UID_ROOT 0
#define GID_WHEEL 0

#define SCM_64                  (1ULL << 30)
#define SCM_SIP                 (2ULL << 24)
#define SCM_CALL(svc, cmd)      ((uint64_t)(svc) << 8 | (cmd))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_MP_ASSIGN           SCM_CALL(0xc, 0x16)
#define SCM_MP_ASSIGN_ARGINFO   0x1117ULL
#define QCOM_VMID_HLOS          3u
#define QCOM_PERM_RW            6u
#define MPSS_DTB_MEM_PHYS       0x9b000000ULL
#define MPSS_FW_MEM_PHYS        0x8ba00000ULL
#ifndef __DECONST
#define __DECONST(t, v) ((t)(uintptr_t)(v))
#endif

static int kernel_map;
static int ipa_lock;
static int annibale_smem_alloc_lock;
static int mpss_glink_lock;
static void lck_mtx_lock(int *l) {{
    if (l == &mpss_glink_lock) {{
        assert(*l == 0); /* catch any self-deadlock on mpss_glink_lock */
        *l = 1;
    }}
}}
static void lck_mtx_unlock(int *l) {{
    if (l == &mpss_glink_lock) {{
        assert(*l == 1);
        *l = 0;
    }}
}}
static void *rootvnode = NULL;
static vfs_context_t vfs_context_kernel(void) {{ return NULL; }}
static int vfs_context_ucred(vfs_context_t c) {{ (void)c; return 0; }}
static void *vfs_context_proc(vfs_context_t c) {{ (void)c; return NULL; }}
static int vnode_lookup(const char *p, int f, vnode_t *vp, vfs_context_t c) {{ (void)p; (void)f; (void)vp; (void)c; return -1; }}
static int vnode_getattr(vnode_t vp, struct vnode_attr *va, vfs_context_t c) {{ (void)vp; (void)va; (void)c; return -1; }}
static void vnode_put(vnode_t vp) {{ (void)vp; }}
static int vn_rdwr(int rw, vnode_t vp, caddr_t buf, int len, off_t off, int seg, int io, int u, int *r, void *p) {{
    (void)rw;(void)vp;(void)buf;(void)len;(void)off;(void)seg;(void)io;(void)u;(void)r;(void)p; return -1;
}}

static void *kalloc_data(size_t s, int f) {{ return (f & Z_ZERO) ? calloc(1, s) : malloc(s); }}
static void kfree_data(void *p, size_t s) {{ (void)s; free(p); }}
__attribute__((unused)) static kern_return_t kmem_alloc(int m, vm_offset_t *va, vm_size_t s, int f, int t) {{
    (void)m;(void)f;(void)t;
    void *p = NULL;
    if (posix_memalign(&p, PAGE_SIZE, s) != 0) return -1;
    memset(p, 0, s);
    *va = (vm_offset_t)p;
    return KERN_SUCCESS;
}}
static kern_return_t kmem_alloc_contig(int m, vm_offset_t *va, vm_size_t s, uint64_t mask,
    uint64_t max_pnum, uint64_t pnum_mask, int f, int t) {{
    (void)m;(void)mask;(void)max_pnum;(void)pnum_mask;(void)f;(void)t;
    void *p = NULL;
    if (posix_memalign(&p, PAGE_SIZE, s) != 0) return -1;
    memset(p, 0, s);
    *va = (vm_offset_t)p;
    return KERN_SUCCESS;
}}
static void kmem_free(int m, vm_offset_t va, vm_size_t s) {{ (void)m;(void)s; free((void *)va); }}
static uint64_t kvtophys(vm_offset_t va) {{ return (uint64_t)va; }}
__attribute__((unused)) static void delay_for_interval(uint32_t i, uint64_t u) {{ (void)i;(void)u; }}
#define NSEC_PER_MSEC 1000000ULL

static const uint8_t *annibale_image_file(const char *d, const char *n, uint64_t *sz, int *do_) {{
    (void)d;(void)n;(void)sz;(void)do_; return NULL;
}}

/* Mocked FAT16 + SMEM/TZ instrumentation */
static struct {{
    char names[8][32];
    const uint8_t *data[8];
    uint64_t sizes[8];
    unsigned count;
}} mock_files;

static uint8_t mock_carveout[128 * 1024];
_Alignas(64) static uint8_t mock_lowdma[192 * 1024];
static uint8_t mock_rmtfs_shm[4 * 1024 * 1024];
static uint8_t mock_memshare_shm[10 * 1024 * 1024];
static bool carveout_mapped, lowdma_enabled;
static unsigned map_calls, unmap_calls, mem_setup_before_map, host_dsb_count;
static unsigned lowdma_map_calls, lowdma_map_fl;
static unsigned cacheclean_calls;
static uintptr_t cacheclean_addrs[16];
static unsigned cacheclean_addr_count;
static uint64_t last_setup_size, last_init_hdr_pas[16];
static unsigned last_init_count;
static bool reject_init_0x82;
static int64_t mock_assign_rc;
static uint64_t mock_assign_res1;
static struct {{
    uint64_t fn, arginfo, a0, a1, a2, a3;
    unsigned cacheclean_before_smc, dsb_before_smc;
}} last_assign_smc;
static bool mock_aoss_off_sent;
static char smc_seq[16][32];
static unsigned smc_count;

static void host_cacheclean(uintptr_t line) {{
    if (cacheclean_addr_count < 16) {{
        cacheclean_addrs[cacheclean_addr_count++] = line;
    }}
    cacheclean_calls++;
}}

static vm_offset_t ml_io_map_unmappable(vm_offset_t pa, vm_size_t sz, unsigned int fl) {{
    if (pa == 0xcf6fc000ULL && sz == sizeof(mock_lowdma)) {{
        lowdma_map_calls++;
        lowdma_map_fl = fl;
        memset(mock_lowdma, 0x55, sz);
        return (vm_offset_t)mock_lowdma;
    }}
    if (pa == 0xcf200000ULL && sz == sizeof(mock_rmtfs_shm)) {{
        return (vm_offset_t)mock_rmtfs_shm;
    }}
    if (pa >= 0xce800000ULL && pa + sz <= 0xcf200000ULL) {{
        return (vm_offset_t)(mock_memshare_shm + (pa - 0xce800000ULL));
    }}
    assert(sz <= sizeof(mock_carveout));
    memset(mock_carveout, 0xaa, sz);
    carveout_mapped = true;
    map_calls++;
    return (vm_offset_t)mock_carveout;
}}
static void ml_io_unmap(vm_offset_t addr, vm_size_t sz) {{
    (void)sz;
    if (addr == (vm_offset_t)mock_lowdma || addr == (vm_offset_t)mock_rmtfs_shm ||
        (addr >= (vm_offset_t)mock_memshare_shm && addr < (vm_offset_t)(mock_memshare_shm + sizeof(mock_memshare_shm)))) {{
        return;
    }}
    assert(addr == (vm_offset_t)mock_carveout && carveout_mapped);
    carveout_mapped = false;
    unmap_calls++;
}}

static bool annibale_ufs_fat16_fw_read(const char *name, uint64_t *size_out, volatile void *dst_buf, uint64_t dst_cap) {{
    for (unsigned i = 0; i < mock_files.count; i++) {{
        if (strcmp(mock_files.names[i], name) == 0) {{
            if (size_out) *size_out = mock_files.sizes[i];
            if (dst_buf != NULL) {{
                if (dst_cap < mock_files.sizes[i]) return false;
                volatile uint8_t *d = (volatile uint8_t *)dst_buf;
                for (uint64_t k = 0; k < mock_files.sizes[i]; k++) d[k] = mock_files.data[i][k];
            }}
            return true;
        }}
    }}
    return false;
}}

static int64_t host_tz_smc(uint64_t fn, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *res1_out) {{
    if (res1_out) *res1_out = 0;
    if (fn == SCM_PAS_INIT_IMAGE) {{
        assert(a3 == 0);
        snprintf(smc_seq[smc_count++], 32, "INIT:%llu:%#llx", a0, arginfo);
        last_init_hdr_pas[last_init_count++] = a1;
        if (reject_init_0x82 && arginfo == (2u | (2u << 6))) {{
            if (res1_out) *res1_out = 1;
            return -22;
        }}
        return 0;
    }}
    if (fn == SCM_PAS_MEM_SETUP) {{
        assert(a3 == 0);
        snprintf(smc_seq[smc_count++], 32, "SETUP:%llu:%#llx", a0, a2);
        if (!carveout_mapped && map_calls == 0) mem_setup_before_map++;
        last_setup_size = a2;
        return 0;
    }}
    if (fn == SCM_PAS_AUTH_AND_RESET) {{
        assert(a3 == 0);
        snprintf(smc_seq[smc_count++], 32, "AUTH:%llu", a0);
        return 0;
    }}
    if (fn == SCM_PAS_SHUTDOWN) {{
        assert(arginfo == 1u && a1 == 0 && a2 == 0 && a3 == 0);
        snprintf(smc_seq[smc_count++], 32, "SHUTDOWN:%llu", a0);
        return 0;
    }}
    if (fn == SCM_MP_ASSIGN) {{
        snprintf(smc_seq[smc_count++], 32, "ASSIGN:%#llx", arginfo);
        last_assign_smc.fn = fn;
        last_assign_smc.arginfo = arginfo;
        last_assign_smc.a0 = a0;
        last_assign_smc.a1 = a1;
        last_assign_smc.a2 = a2;
        last_assign_smc.a3 = a3;
        last_assign_smc.cacheclean_before_smc = cacheclean_calls;
        last_assign_smc.dsb_before_smc = host_dsb_count;
        if (res1_out) *res1_out = mock_assign_res1;
        return mock_assign_rc;
    }}
    return -1;
}}

{ipa_loader_slice}
{sanitize_fn}

/* Modem init modem=0 harness */
static uint32_t modem_boot_arg;
static bool modem_inited, worker_started;
static int modem_cdevsw;
static bool annibale_platform_is_annibale(void) {{ return true; }}
static int PE_parse_boot_argn(const char *k, void *out, unsigned sz) {{
    if (strcmp(k, "modem") == 0 && sz == 4) {{
        uint32_t z = 0;
        memcpy(out, &z, 4);
        return 1;
    }}
    if (lowdma_enabled && strcmp(k, "lowdma") == 0 && sz == 8) {{
        uint64_t v = 0xc772c000ULL;
        memcpy(out, &v, 8);
        return 1;
    }}
    if (lowdma_enabled && strcmp(k, "lowdma_mb") == 0 && sz == 8) {{
        uint64_t v = 128ULL;
        memcpy(out, &v, 8);
        return 1;
    }}
    return 0;
}}
static int cdevsw_add(int a, const void *b) {{ (void)a;(void)b; return 1; }}
static int makedev(int a, int b) {{ (void)a;(void)b; return 1; }}
static void *devfs_make_node(int d, int c, int u, int g, int m, const char *n) {{
    (void)d;(void)c;(void)u;(void)g;(void)m;(void)n; return (void *)1;
}}
static void modem_sync_sysctls(void) {{}}
static void modem_worker_thread(void *a, int w) {{ (void)a;(void)w; }}
static kern_return_t kernel_thread_start(void (*fn)(void *, int), void *a, thread_t *th) {{
    (void)fn;(void)a;(void)th; worker_started = true; return 0;
}}
static void thread_deallocate(thread_t t) {{ (void)t; }}
{init_fn}

/* ---- 5. SMEM GLINK handshake, desc offsets, intents & reassembly harness ---- */
#define SM8750_SMEM_SIZE                0x00040000ULL
#define SMEM_HOST_APPS                  0u
#define SMEM_HOST_MPSS                  1u
#define SMEM_HOST_GLOBAL                0xfffeu
#define SMEM_CANARY                     0xa5a5u
#define ITEM_MPSS_CRASH_REASON          421u
#define ITEM_SMP2P_MPSS_OURS            428u
#define ITEM_SMP2P_MPSS_THEIRS          435u
#define ITEM_GLINK_DESCRIPTOR           478u
#define ITEM_GLINK_OUR_QUEUE            479u
#define ITEM_GLINK_THEIR_QUEUE          480u
#define GLINK_QUEUE_SIZE                16384u
#define SMP2P_MAGIC                     0x504d5324u
#define SMP2P_VERSION                   1u
#define SMP2P_SSR_ACK                   (1u << 0)
#define IPCC_REG_SEND                   0x0c
#define IPCC_CLIENT_MPSS                2u
#define IPCC_SIG_GLINK                  0u
#define IPCC_SIG_SMP2P                  2u
#define GLINK_CMD_VERSION               0u
#define GLINK_CMD_VERSION_ACK           1u
#define GLINK_CMD_OPEN                  2u
#define GLINK_CMD_CLOSE                 3u
#define GLINK_CMD_OPEN_ACK              4u
#define GLINK_CMD_INTENT                5u
#define GLINK_CMD_RX_DONE               6u
#define GLINK_CMD_RX_INTENT_REQ         7u
#define GLINK_CMD_RX_INTENT_REQ_ACK     8u
#define GLINK_CMD_TX_DATA               9u
#define GLINK_CMD_TX_SHORT_DATA         10u
#define GLINK_CMD_CLOSE_ACK             11u
#define GLINK_CMD_TX_DATA_CONT          12u
#define GLINK_CMD_READ_NOTIF            13u
#define GLINK_CMD_RX_DONE_REUSE         14u
#define GLINK_CMD_SIGNALS               15u
#define GLINK_FEATURE_INTENT_REUSE      (1u << 0)
#define GLINK_OUR_CID                   1u
#define MPSS_GLINK_MAX_INTENTS          32u
#define MPSS_GLINK_REASM_MAX            8192u

static uint32_t mock_smem[SM8750_SMEM_SIZE / 4u];
static volatile uint32_t *smem_base = mock_smem;
static uint32_t mock_ipcc_regs[16];
static vm_offset_t ipcc_base = (vm_offset_t)mock_ipcc_regs;
static unsigned ipcc_rings;
static uint32_t ipcc_last_val;
static uint8_t qrtr_rx_buf[MPSS_GLINK_REASM_MAX];
static uint32_t qrtr_rx_len, qrtr_rx_count;
static bool qrtr_rx_echo_tx;
static int qrtr_rx_echo_rc;

static inline uint32_t smem_rd(uint32_t byte_off) {{
    assert((byte_off & 3u) == 0 && (uint64_t)byte_off + 4u <= SM8750_SMEM_SIZE);
    return smem_base[byte_off / 4u];
}}
static inline void smem_wr(uint32_t byte_off, uint32_t v) {{
    assert((byte_off & 3u) == 0 && (uint64_t)byte_off + 4u <= SM8750_SMEM_SIZE);
    smem_base[byte_off / 4u] = v;
}}
static inline void mmio_wr(vm_offset_t addr, uint32_t v) {{
    *(volatile uint32_t *)addr = v;
    if (addr == ipcc_base + IPCC_REG_SEND) {{
        ipcc_rings++;
        ipcc_last_val = v;
    }}
}}
static void annibale_qrtr_input(const void *data, uint32_t length);

{glink_state_src}
{boot_glink_src}
{stall_diag_src}
{modem_glink_src}
#define MPSS_DTB_PAS_ID 38u
#define MPSS_FW_PAS_ID  4u
#define SMP2P_MPSS_FATAL (1u << 0)
#define SMP2P_MPSS_READY (1u << 1)
static void modem_sleep_ms(uint32_t ms) {{ (void)ms; }}
static bool annibale_aoss_send_msg(const char *msg) {{
    if (strstr(msg, "val: off") != NULL) mock_aoss_off_sent = true;
    return true;
}}
{boot_tail_src}

static void annibale_qrtr_input(const void *data, uint32_t length) {{
    assert(mpss_glink_lock == 0); /* must NOT hold mpss_glink_lock across QRTR upcall */
    assert(length <= sizeof(qrtr_rx_buf));
    memcpy(qrtr_rx_buf, data, length);
    qrtr_rx_len = length;
    qrtr_rx_count++;
    if (qrtr_rx_echo_tx) {{
        static const uint8_t reply[32] = {{ 1, 2, 3, 4, 5, 6, 7, 8 }};
        qrtr_rx_echo_rc = mpss_qrtr_send(reply, sizeof(reply));
    }}
}}

static void mock_smem_init_partition(void) {{
    memset(mock_smem, 0, sizeof(mock_smem));
    uint32_t toc_off = (uint32_t)SM8750_SMEM_SIZE - 0x1000u;
    uint32_t part_off = 0x1000u, part_sz = 0x20000u, g_off = 0x22000u, g_sz = 0x8000u;
    smem_wr(toc_off + 0u, 0x434f5424u); /* $TOC */
    smem_wr(toc_off + 8u, 2u);
    smem_wr(toc_off + 32u + 0u, part_off); smem_wr(toc_off + 32u + 4u, part_sz);
    smem_wr(toc_off + 32u + 12u, (uint32_t)SMEM_HOST_APPS | ((uint32_t)SMEM_HOST_MPSS << 16));
    smem_wr(toc_off + 32u + 16u, 64u);
    smem_wr(part_off + 0u, 0x54525024u); smem_wr(part_off + 12u, 32u); smem_wr(part_off + 16u, part_sz);
    /* Populate SMEM_HOST_GLOBAL with a competing ITEM_GLINK_THEIR_QUEUE (480) to prove isolation. */
    uint32_t e2 = toc_off + 32u + 48u;
    smem_wr(e2 + 0u, g_off); smem_wr(e2 + 4u, g_sz);
    smem_wr(e2 + 12u, (uint32_t)SMEM_HOST_GLOBAL | ((uint32_t)SMEM_HOST_GLOBAL << 16));
    smem_wr(e2 + 16u, 64u);
    smem_wr(g_off + 0u, 0x54525024u); smem_wr(g_off + 12u, 32u + 16u + GLINK_QUEUE_SIZE); smem_wr(g_off + 16u, g_sz);
    smem_wr(g_off + 32u, SMEM_CANARY | (ITEM_GLINK_THEIR_QUEUE << 16));
    smem_wr(g_off + 36u, GLINK_QUEUE_SIZE);
}}

static void mpss_push_rx(uint16_t cmd, uint16_t p1, uint32_t p2, const void *payload, uint32_t len) {{
    uint32_t aligned = (8u + len + 7u) & ~7u;
    uint32_t head = smem_rd(mpss_glink.desc_off + 12u);
    uint8_t hdr[8] = {{
        (uint8_t)(cmd & 0xffu), (uint8_t)(cmd >> 8),
        (uint8_t)(p1 & 0xffu), (uint8_t)(p1 >> 8),
        0, 0, 0, 0
    }};
    memcpy(hdr + 4, &p2, 4u);
    for (uint32_t i = 0; i < aligned; i += 4u) {{
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4u; b++) {{
            uint32_t idx = i + b;
            uint8_t byte = idx < 8u ? hdr[idx] :
                (payload != NULL && idx - 8u < len ? ((const uint8_t *)payload)[idx - 8u] : 0u);
            w |= (uint32_t)byte << (b * 8u);
        }}
        smem_wr(mpss_glink.their_q_off + head, w);
        head = (head + 4u) % mpss_glink.their_q_sz;
    }}
    smem_wr(mpss_glink.desc_off + 12u, head);
}}

struct tx_cmd {{ uint16_t cmd, p1; uint32_t p2; uint8_t data[256]; uint32_t data_len; }};
static bool mpss_pop_tx(struct tx_cmd *out) {{
    uint32_t tail = smem_rd(mpss_glink.desc_off + 0u);
    uint32_t head = smem_rd(mpss_glink.desc_off + 4u);
    if (tail == head) return false;
    uint32_t w0 = smem_rd(mpss_glink.our_q_off + tail);
    uint32_t w1 = smem_rd(mpss_glink.our_q_off + ((tail + 4u) % mpss_glink.our_q_sz));
    memset(out, 0, sizeof(*out));
    out->cmd = (uint16_t)(w0 & 0xffffu);
    out->p1 = (uint16_t)(w0 >> 16);
    out->p2 = w1;
    uint32_t extra = 0;
    if (out->cmd == GLINK_CMD_OPEN) extra = ((out->p2 & 0xffffu) + 7u) & ~7u;
    else if (out->cmd == GLINK_CMD_INTENT) extra = out->p2 * 8u;
    else if (out->cmd == GLINK_CMD_TX_DATA) {{
        uint32_t fsz = smem_rd(mpss_glink.our_q_off + ((tail + 8u) % mpss_glink.our_q_sz));
        extra = 8u + ((fsz + 7u) & ~7u);
    }}
    tail = (tail + 8u) % mpss_glink.our_q_sz;
    out->data_len = extra < sizeof(out->data) ? extra : (uint32_t)sizeof(out->data);
    for (uint32_t i = 0; i < out->data_len; i += 4u) {{
        uint32_t w = smem_rd(mpss_glink.our_q_off + ((tail + i) % mpss_glink.our_q_sz));
        memcpy(out->data + i, &w, 4u);
    }}
    tail = (tail + extra) % mpss_glink.our_q_sz;
    smem_wr(mpss_glink.desc_off + 0u, tail);
    return true;
}}

static void test_glink_mpss(void) {{
    struct tx_cmd tx;

    /* 5a. SMP2P init matches smp2p.ko header (version=1, features=SSR_ACK=1 -> 0x101, flags=0);
     * Pre-boot GLINK allocates desc + our_q and zeroes all 32B of desc without sending VERSION yet. */
    mock_smem_init_partition();
    mock_ipcc_regs[0] = 8u;
    mock_ipcc_regs[1] = 0x00020100u;
    mock_ipcc_regs[2] = 0u;
    ipcc_rings = 0;
    smp2p_mpss_init_ours();
    uint32_t smp_sz = 0, smp_at = smem_find_mpss_item(ITEM_SMP2P_MPSS_OURS, &smp_sz);
    assert(smp_at != 0 && smp_sz >= sizeof(struct modem_smp2p_item));
    assert(smem_rd(smp_at + 0u) == SMP2P_MAGIC);
    assert(smem_rd(smp_at + 4u) == 0x00000101u);
    assert(smem_rd(smp_at + 16u) == 0u);
    assert(ipcc_rings == 1 && ipcc_last_val == ((IPCC_CLIENT_MPSS << 16) | IPCC_SIG_SMP2P));

    ipcc_rings = 0;
    mpss_glink_init_preboot();
    assert(mpss_glink.desc_off != 0 && mpss_glink.our_q_off != 0 && !mpss_glink.version_sent);
    for (uint32_t off = 0; off < 32u; off += 4u) {{
        assert(smem_rd(mpss_glink.desc_off + off) == 0u);
    }}
    assert(ipcc_rings == 0);

    /* 5b. locate_queues stays false until MPSS creates item 480 (their_q), then sends initial VERSION(1,1) + rings IPCC(2,0). */
    assert(!mpss_glink_locate_queues());
    uint32_t their_q = smem_alloc_mpss_item(ITEM_GLINK_THEIR_QUEUE, GLINK_QUEUE_SIZE);
    assert(their_q != 0);
    assert(mpss_glink_locate_queues());
    assert(mpss_glink.their_q_off == their_q && mpss_glink.their_q_sz == GLINK_QUEUE_SIZE && mpss_glink.version_sent);
    assert(smem_rd(mpss_glink.desc_off + 0u) == 0u);  /* our_tail */
    assert(smem_rd(mpss_glink.desc_off + 4u) == 8u);  /* our_head */
    assert(smem_rd(mpss_glink.desc_off + 8u) == 0u);  /* their_tail */
    assert(smem_rd(mpss_glink.desc_off + 12u) == 0u); /* their_head */
    assert(ipcc_rings == 1 && ipcc_last_val == ((IPCC_CLIENT_MPSS << 16) | IPCC_SIG_GLINK));
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_VERSION && tx.p1 == 1u && tx.p2 == GLINK_FEATURE_INTENT_REUSE);
    assert(smem_rd(mpss_glink.desc_off + 0u) == 8u);
    assert(!mpss_pop_tx(&tx));

    /* 5c. Full MPSS handshake: VERSION(0x9), VERSION_ACK(1), OPEN(DIAG w/ hi16), OPEN(IPCRTR non-NUL), OPEN_ACK, trailing TX_DATA. */
    mpss_push_rx(GLINK_CMD_VERSION, 1u, 0x9u, NULL, 0);
    mpss_push_rx(GLINK_CMD_VERSION_ACK, 1u, 1u, NULL, 0);
    static const char diag_name[5] = "DIAG";
    mpss_push_rx(GLINK_CMD_OPEN, 3u, (0x0005u << 16) | 5u, diag_name, 5u);
    static const uint8_t ipcrtr_dirty[8] = {{ 'I','P','C','R','T','R', 0xde, 0xad }};
    mpss_push_rx(GLINK_CMD_OPEN, 7u, (0x0002u << 16) | 6u, ipcrtr_dirty, 8u);
    mpss_push_rx(GLINK_CMD_OPEN_ACK, GLINK_OUR_CID, 0u, NULL, 0);
    static const uint8_t early_tx[16] = {{ 8,0,0,0, 0,0,0,0, 'H','E','L','L','O','!','!','!' }};
    mpss_push_rx(GLINK_CMD_TX_DATA, 7u, 1u, early_tx, sizeof(early_tx));

    mpss_glink_poll_rx();
    assert(mpss_glink.agreed && mpss_glink.their_cid == 7u && mpss_glink.ipcrtr_open);
    /* Must break out of poll_rx right on ipcrtr_open so QRTR attaches BEFORE early_tx is delivered. */
    assert(qrtr_rx_count == 0);
    assert(smem_rd(mpss_glink.desc_off + 8u) < smem_rd(mpss_glink.desc_off + 12u));

    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_VERSION_ACK && tx.p1 == 1u && tx.p2 == 1u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_OPEN && tx.p1 == GLINK_OUR_CID && tx.p2 == 7u && memcmp(tx.data, "IPCRTR", 7) == 0);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_OPEN_ACK && tx.p1 == 3u && tx.p2 == 0u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_OPEN_ACK && tx.p1 == 7u && tx.p2 == 0u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_INTENT && tx.p1 == GLINK_OUR_CID && tx.p2 == 10u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_RX_INTENT_REQ && tx.p1 == GLINK_OUR_CID && tx.p2 == 0x800u);
    assert(!mpss_pop_tx(&tx));

    /* Next poll consumes early_tx and returns RX_DONE_REUSE(lcid=1, iid=1). */
    mpss_glink_poll_rx();
    assert(qrtr_rx_count == 1 && qrtr_rx_len == 8u && memcmp(qrtr_rx_buf, "HELLO!!!", 8) == 0);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_RX_DONE_REUSE && tx.p1 == GLINK_OUR_CID && tx.p2 == 1u);

    /* 5d. Arbitrary remote intents (>16), TX_SHORT_DATA(24B), SIGNALS, READ_NOTIF, RX_INTENT_REQ, multi-frag reassembly + re-entrant mpss_qrtr_send. */
    uint32_t remote_intents[4] = {{ 256u, 25u, 4096u, 0x1001u }};
    mpss_push_rx(GLINK_CMD_INTENT, 7u, 2u, remote_intents, sizeof(remote_intents));
    uint8_t short_body[16] = {{ 0xaa }};
    mpss_push_rx(GLINK_CMD_TX_SHORT_DATA, 3u, 0x99u, short_body, sizeof(short_body));
    mpss_push_rx(GLINK_CMD_SIGNALS, 7u, 0x1u, NULL, 0);
    mpss_push_rx(GLINK_CMD_READ_NOTIF, 0u, 0u, NULL, 0);
    mpss_push_rx(GLINK_CMD_RX_INTENT_REQ, 7u, 0x5000u, NULL, 0);

    uint8_t frag1[8 + 100], frag2[8 + 60];
    uint32_t f1_sz = 100u, f1_left = 60u, f2_sz = 60u, f2_left = 0u;
    memcpy(frag1 + 0, &f1_sz, 4u); memcpy(frag1 + 4, &f1_left, 4u);
    for (uint32_t i = 0; i < 100u; i++) frag1[8u + i] = (uint8_t)(i + 1u);
    memcpy(frag2 + 0, &f2_sz, 4u); memcpy(frag2 + 4, &f2_left, 4u);
    for (uint32_t i = 0; i < 60u; i++) frag2[8u + i] = (uint8_t)(101u + i);
    mpss_push_rx(GLINK_CMD_TX_DATA, 7u, 5u, frag1, sizeof(frag1));
    mpss_push_rx(GLINK_CMD_TX_DATA_CONT, 7u, 5u, frag2, sizeof(frag2));

    qrtr_rx_echo_tx = true;
    qrtr_rx_echo_rc = -1;
    mpss_glink_poll_rx();
    qrtr_rx_echo_tx = false;

    assert(mpss_glink.their_intent_count == 2u);
    assert(mpss_glink_has_free_intent(64u));
    assert(mpss_glink_has_free_intent(4096u));
    assert(!mpss_glink_has_free_intent(4097u));
    assert(qrtr_rx_count == 2 && qrtr_rx_len == 160u && qrtr_rx_echo_rc == 0);
    for (uint32_t i = 0; i < 160u; i++) assert(qrtr_rx_buf[i] == (uint8_t)(i + 1u));

    /* Expect: extra INTENT(0x5000), RX_INTENT_REQ_ACK(lcid=1,1), RX_DONE_REUSE(lcid=1,5), TX_DATA(lcid=1, iid=25). */
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_INTENT && tx.p1 == GLINK_OUR_CID && tx.p2 == 1u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_RX_INTENT_REQ_ACK && tx.p1 == GLINK_OUR_CID && tx.p2 == 1u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_RX_DONE_REUSE && tx.p1 == GLINK_OUR_CID && tx.p2 == 5u);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_TX_DATA && tx.p1 == GLINK_OUR_CID && tx.p2 == 25u);
    assert(!mpss_pop_tx(&tx));

    /* MPSS returns RX_DONE_REUSE(rcid=7, iid=25) -> intent 25 becomes free again (second small send reuses 25). */
    mpss_push_rx(GLINK_CMD_RX_DONE_REUSE, 7u, 25u, NULL, 0);
    mpss_glink_poll_rx();
    static const uint8_t msg2[16] = {{ 0x5a }};
    assert(mpss_qrtr_send(msg2, sizeof(msg2)) == 0);
    assert(mpss_pop_tx(&tx) && tx.cmd == GLINK_CMD_TX_DATA && tx.p1 == GLINK_OUR_CID && tx.p2 == 25u);

    /* 5e. Queue-full guard without unsigned underflow when tail == head + 4; stall diag exercises SMEM+IPCC reads. */
    smem_wr(mpss_glink.desc_off + 4u, 100u); smem_wr(mpss_glink.desc_off + 0u, 104u);
    assert(!mpss_glink_write_raw(GLINK_CMD_SIGNALS, GLINK_OUR_CID, 0u, NULL, 0));
    assert(smem_rd(mpss_glink.desc_off + 4u) == 100u);
    mpss_glink_log_stall_diag(10u);

    /* 5f. Worker GLINK tick polls past timeout & PAS_SHUTDOWN lifecycle (stop + reboot quiesce + modem=0 no-op). */
    bool s_seen = false, n_open = false, r_hello = false, is_op = false;
    uint32_t s_v = 0, s_f = 0, rc_id = 0, in_cnt = 0;
    modem_worker_glink_tick(12u, &s_seen, &s_v, &s_f, &n_open, &r_hello, &is_op, &rc_id, &in_cnt);
    assert(is_op && n_open);
    modem_worker_glink_tick(13u, &s_seen, &s_v, &s_f, &n_open, &r_hello, &is_op, &rc_id, &in_cnt);
    assert(is_op && !n_open);

    modem_boot_arg = 3u; mpss_pas_fw_authed = true; mpss_pas_dtb_authed = true; mpss_stopping = false;
    smc_count = 0; ipcc_rings = 0; mock_aoss_off_sent = false;
    annibale_modem_stop();
    assert(mpss_stopping && !mpss_pas_fw_authed && !mpss_pas_dtb_authed && mock_aoss_off_sent);
    assert(smc_count == 2 && strcmp(smc_seq[0], "SHUTDOWN:4") == 0 && strcmp(smc_seq[1], "SHUTDOWN:38") == 0);
    assert((smem_rd(smp_at + 36u) & 1u) != 0 && ipcc_rings >= 1);
    assert(annibale_modem_return_quiesce() && smc_count == 2); /* no-op once already stopped */

    mpss_pas_fw_authed = true; mpss_pas_dtb_authed = true; mpss_stopping = false; smc_count = 0;
    assert(annibale_modem_return_quiesce() && !mpss_pas_fw_authed && !mpss_pas_dtb_authed);
    assert(smc_count == 2 && strcmp(smc_seq[0], "SHUTDOWN:4") == 0 && strcmp(smc_seq[1], "SHUTDOWN:38") == 0);

    modem_boot_arg = 0u; mpss_pas_fw_authed = true; smc_count = 0;
    assert(annibale_modem_return_quiesce() && smc_count == 0);
}}

/* ---- 6. APPS-side QMI servers (RMTFS 0x0e, MEMSHARE 0x34, SERVREG_LOCATOR 0x40) + QMI_LOC ---- */
#define MPSS_VMID_MSA 15u
static inline uint16_t get_16(const uint8_t *p) {{ return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }}
static inline void put_16(uint8_t *p, uint16_t v) {{ p[0] = (uint8_t)(v & 0xffu); p[1] = (uint8_t)(v >> 8); }}
{qmi_codec_src}

struct annibale_qrtr_port {{ uint32_t id; }};
typedef void (*annibale_qrtr_receive_t)(void *, uint32_t, uint32_t, const void *, size_t);
typedef void (*annibale_qrtr_server_t)(void *, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static struct annibale_qrtr_port svc_mock_ports[8];
static unsigned svc_mock_port_n, svc_pub_n, ufs_rd_calls;
static struct {{ uint32_t svc, ver, inst; }} svc_pubs[8];
static uint8_t svc_last_resp[1024];
static size_t svc_last_resp_len;
static uint32_t svc_last_node, svc_last_port;

static struct annibale_qrtr_port *svc_qrtr_open(annibale_qrtr_receive_t r, annibale_qrtr_server_t s, void *c) {{
    (void)r;(void)s;(void)c;
    struct annibale_qrtr_port *p = &svc_mock_ports[svc_mock_port_n++];
    p->id = 0x4000u + svc_mock_port_n;
    return p;
}}
static int svc_qrtr_send(struct annibale_qrtr_port *p, uint32_t node, uint32_t to, const void *d, size_t l) {{
    (void)p;
    svc_last_node = node;
    svc_last_port = to;
    svc_last_resp_len = l < sizeof(svc_last_resp) ? l : sizeof(svc_last_resp);
    memcpy(svc_last_resp, d, svc_last_resp_len);
    return 0;
}}
static const struct {{
    struct annibale_qrtr_port *(*open)(annibale_qrtr_receive_t, annibale_qrtr_server_t, void *);
    int (*send)(struct annibale_qrtr_port *, uint32_t, uint32_t, const void *, size_t);
}} annibale_qrtr = {{ .open = svc_qrtr_open, .send = svc_qrtr_send }};

static int annibale_qrtr_publish(struct annibale_qrtr_port *p, uint32_t svc, uint32_t ver, uint32_t inst) {{
    (void)p;
    svc_pubs[svc_pub_n].svc = svc;
    svc_pubs[svc_pub_n].ver = ver;
    svc_pubs[svc_pub_n].inst = inst;
    svc_pub_n++;
    return 0;
}}

static int annibale_ufs_part_read(const char *part_name, uint64_t off, void *buf, uint64_t len) {{
    if (strcmp(part_name, "modemst1") != 0 && strcmp(part_name, "modemst2") != 0 &&
        strcmp(part_name, "fsg") != 0 && strcmp(part_name, "fsc") != 0) return ENOENT;
    ufs_rd_calls++;
    uint8_t *out = buf;
    for (uint64_t i = 0; i < len; i++) out[i] = (uint8_t)(((off + i) ^ 0xa5u) & 0xffu);
    return 0;
}}

{svc_host_src}

static void test_modem_qmi_services(void) {{
    uint8_t req[256], tlvs[128];
    size_t tp = 0, rn;
    uint16_t tl = 0;
    const uint8_t *tv;

    lowdma_enabled = true;
    modem_start_local_services();
    assert(svc_pub_n == 3);
    assert(svc_pubs[0].svc == 0x0eu && svc_pubs[0].ver == 1u && svc_pubs[0].inst == 0u);
    assert(svc_pubs[1].svc == 0x34u && svc_pubs[1].ver == 1u && svc_pubs[1].inst == 1u);
    assert(svc_pubs[2].svc == 0x40u && svc_pubs[2].ver == 1u && svc_pubs[2].inst == 1u);

    /* 6a. SERVREG_LOCATOR GET_DOMAIN_LIST for "tms/servreg" + pagination + "gps/gps_service" + unknown. */
    tp = 0; svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x01u, "tms/servreg", 11u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 7u, QMI_SERVREG_GET_DOMAIN_LIST, tlvs, (uint16_t)tp);
    svc_servreg_rx(NULL, 0u, 0x11u, req, rn);
    assert(svc_last_resp_len > 7u && svc_last_resp[0] == 0x02u && get_16(svc_last_resp + 1) == 7u);
    assert(get_16(svc_last_resp + 3) == QMI_SERVREG_GET_DOMAIN_LIST);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x02u, &tl);
    assert(tv != NULL && tl == 4u && get_16(tv) == 0u && get_16(tv + 2) == 0u);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    assert(tv != NULL && tl == 2u && get_16(tv) == 7u);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x12u, &tl);
    assert(tv != NULL && tv[0] == 7u && tv[1] == 17u && memcmp(tv + 2, "msm/modem/root_pd", 17u) == 0);
    uint32_t inst0 = 0; memcpy(&inst0, tv + 19, 4u);
    assert(inst0 == 180u && tv[23] == 0u);

    tp = 0; svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x01u, "tms/servreg", 11u);
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x10u, 2u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 8u, QMI_SERVREG_GET_DOMAIN_LIST, tlvs, (uint16_t)tp);
    svc_servreg_rx(NULL, 0u, 0x11u, req, rn);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x12u, &tl);
    assert(tv != NULL && tv[0] == 5u);

    tp = 0; svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x01u, "no/such", 7u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 9u, QMI_SERVREG_GET_DOMAIN_LIST, tlvs, (uint16_t)tp);
    svc_servreg_rx(NULL, 0u, 0x11u, req, rn);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    assert(tv != NULL && get_16(tv) == 0u);
    assert(annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x12u, &tl) == NULL);

    /* 6b. MEMSHARE QUERY_SIZE + exclusive ALLOC(cid=1, 5 MiB) + idempotent FREE/re-ALLOC + shared ALLOC(cid=6). */
    tp = 0; svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 1u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 10u, QMI_MEM_QUERY_SIZE, tlvs, (uint16_t)tp);
    svc_memshare_rx(NULL, 0u, 0x22u, req, rn);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    uint32_t qsz = 0; assert(tv != NULL && tl == 4u); memcpy(&qsz, tv, 4u); assert(qsz == 0x500000u);

    smc_count = 0; tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 0x500000u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x02u, 1u);
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x03u, 0u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x04u, 42u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 11u, QMI_MEM_ALLOC_GENERIC, tlvs, (uint16_t)tp);
    svc_memshare_rx(NULL, 0u, 0x22u, req, rn);
    assert(smc_count == 1 && strcmp(smc_seq[0], "ASSIGN:0x1117") == 0);
    uint64_t ext_v[7]; memcpy(ext_v, mock_lowdma + 2u * IPA_TZ_HDR_SLOT_SZ + 0x100u, sizeof(ext_v));
    assert(ext_v[2] == 24ULL);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x11u, &tl);
    assert(tv != NULL && tl == 13u && tv[0] == 1u);
    uint64_t c1_pa = 0; uint32_t c1_sz = 0; memcpy(&c1_pa, tv + 1, 8u); memcpy(&c1_sz, tv + 9, 4u);
    assert(c1_pa == 0xce800000ULL && c1_sz == 0x500000u);

    tp = 0; svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x10u, 1u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 12u, QMI_MEM_FREE_GENERIC, tlvs, (uint16_t)tp);
    svc_memshare_rx(NULL, 0u, 0x22u, req, rn);
    smc_count = 0; tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 0x500000u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x02u, 1u);
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x03u, 0u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x04u, 43u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 13u, QMI_MEM_ALLOC_GENERIC, tlvs, (uint16_t)tp);
    svc_memshare_rx(NULL, 0u, 0x22u, req, rn);
    assert(smc_count == 0);

    tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 0x400000u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x02u, 6u);
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x03u, 0u); svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x04u, 44u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 14u, QMI_MEM_ALLOC_GENERIC, tlvs, (uint16_t)tp);
    svc_memshare_rx(NULL, 0u, 0x22u, req, rn);
    assert(smc_count == 1);
    memcpy(ext_v, mock_lowdma + 2u * IPA_TZ_HDR_SLOT_SZ + 0x100u, sizeof(ext_v));
    assert(ext_v[2] == 48ULL);

    /* 6c. RMTFS OPEN -> ALLOC_BUFF (shared dst_sz=48 at 0xcf200000) -> RW_IOVEC read & RAM-shadow write -> CLOSE. */
    tp = 0; svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x01u, "/boot/modem_fs1", 15u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 20u, QMI_RMTFS_OPEN, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    uint32_t rmt_cid = 0; assert(tv != NULL && tl == 4u); memcpy(&rmt_cid, tv, 4u); assert(rmt_cid == 1u);

    smc_count = 0; tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, rmt_cid);
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x02u, 0x400000u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 21u, QMI_RMTFS_ALLOC_BUFF, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    assert(smc_count == 1);
    memcpy(ext_v, mock_lowdma + 2u * IPA_TZ_HDR_SLOT_SZ + 0x100u, sizeof(ext_v));
    assert(ext_v[2] == 48ULL);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    uint64_t rmt_pa = 0; assert(tv != NULL && tl == 8u); memcpy(&rmt_pa, tv, 8u); assert(rmt_pa == 0xcf200000ULL);

    uint8_t iov[13] = {{ 1u }}; uint32_t iov_w[3] = {{ 2u, 0xcf201000u, 2u }};
    memcpy(iov + 1, iov_w, 12u); tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, rmt_cid); svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x02u, 0u);
    svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x03u, iov, sizeof(iov)); svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x04u, 0u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 22u, QMI_RMTFS_RW_IOVEC, tlvs, (uint16_t)tp);
    ufs_rd_calls = 0; svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    assert(ufs_rd_calls > 0 && mock_rmtfs_shm[0x1000] == (uint8_t)((1024u ^ 0xa5u) & 0xffu));

    memset(mock_rmtfs_shm + 0x2000, 0x5c, 512u);
    uint32_t iov_w2[3] = {{ 2u, 0xcf202000u, 1u }};
    memcpy(iov + 1, iov_w2, 12u); tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, rmt_cid); svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x02u, 1u);
    svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x03u, iov, sizeof(iov)); svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x04u, 1u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 23u, QMI_RMTFS_RW_IOVEC, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    assert(rmtfs_parts[0].shadow != NULL && rmtfs_parts[0].shadow_loaded);

    /* Read back sector 2 & 3 into 0xcf203000: sector 2 == 0x5c (RAM shadow), sector 3 == UFS original! */
    uint32_t iov_w3[3] = {{ 2u, 0xcf203000u, 2u }};
    memcpy(iov + 1, iov_w3, 12u);
    tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, rmt_cid);
    svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x02u, 0u);
    svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x03u, iov, sizeof(iov));
    svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x04u, 0u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 24u, QMI_RMTFS_RW_IOVEC, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    assert(mock_rmtfs_shm[0x3000] == 0x5c && mock_rmtfs_shm[0x3200] == (uint8_t)((1536u ^ 0xa5u) & 0xffu));

    /* GET_DEV_ERROR + CLOSE + DMS/UIM/LOC decoder checks + clean free of shadow buffer for ASan/LSan. */
    tp = 0;
    svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, rmt_cid);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 25u, QMI_RMTFS_GET_DEV_ERROR, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    tv = annibale_qmi_find_tlv(svc_last_resp + 7, svc_last_resp_len - 7, 0x10u, &tl);
    assert(tv != NULL && tl == 1u && tv[0] == 0u);
    rn = annibale_qmi_encode_msg(req, sizeof(req), 0x00u, 26u, QMI_RMTFS_CLOSE, tlvs, (uint16_t)tp);
    svc_rmtfs_rx(NULL, 0u, 0x33u, req, rn);
    assert(!rmtfs_callers[rmt_cid].open);

    /* 6e. DMS GET_OPERATING_MODE (offline_reason + hw_restr) & SET_OPERATING_MODE result/requery. */
    {{
        uint8_t mode = 0xffu;
        bool want_online = false, requery = false;
        tp = 0;
        svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x01u, 3u);
        svc_tlv_put_u16(tlvs, sizeof(tlvs), &tp, 0x10u, 0x0042u);
        svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x11u, 1u);
        modem_dms_handle_rx(0x02u, 0x002du, tlvs, (uint16_t)tp, &mode, &want_online, &requery);
        assert(mode == 3u && want_online && !requery);

        want_online = false;
        tp = 0;
        svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 1u, 0x003au);
        modem_dms_handle_rx(0x02u, 0x002eu, tlvs, (uint16_t)tp, &mode, &want_online, &requery);
        assert(requery && !want_online);
    }}

    /* 6f. UIM GET_CARD_STATUS TLV 0x10 & WMS SET_EVENT_REPORT rsp vs IND. */
    {{
        static const uint8_t uim_payload[] = {{
            0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x10, 0x11, 0x00,
            0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x07
        }};
        uint32_t sim_st = 99u, sms_rx = 0, sms_tx = 0;
        modem_uim_handle_rx(0x02u, 0x002fu, uim_payload, (uint16_t)sizeof(uim_payload), &sim_st);
        assert(sim_st == 2u);

        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 0u, 0u);
        modem_wms_handle_rx(0x02u, 0x0001u, tlvs, (uint16_t)tp, &sms_rx, &sms_tx);
        assert(sms_rx == 0u);
        modem_wms_handle_rx(0x04u, 0x0001u, tlvs, (uint16_t)tp, &sms_rx, &sms_tx);
        assert(sms_rx == 1u);

        uint32_t call_st = 0;
        const uint8_t voice_ind[] = {{ 0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x02u }};
        modem_voice_handle_rx(0x002eu, voice_ind, (uint16_t)sizeof(voice_ind), &call_st);
        assert(call_st == 2u);
        modem_voice_handle_rx(0x0021u, NULL, 0u, &call_st);
        assert(call_st == 0u);
    }}

    /* 6g. QMI_LOC GET_SUPPORTED_MSGS/FIELDS, INFORM_CLIENT_REV, REG_EVENTS fallback, & indications. */
    {{
        bool rev_done = false, reg_ok = false, retry_min = false;
        static const uint8_t sup_msgs[24] = {{ 0x16, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff }};
        static const uint8_t sup_flds[4] = {{ 0x02, 0x01, 0x00, 0x00 }};
        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 0u, 0u);
        svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x10u, sup_msgs, (uint16_t)sizeof(sup_msgs));
        modem_loc_handle_rx(0x02u, 11u, 0x001eu, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);
        assert(!rev_done);
        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 0u, 0u);
        svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x10u, sup_flds, (uint16_t)sizeof(sup_flds));
        modem_loc_handle_rx(0x02u, 12u, 0x001fu, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);
        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 0u, 0u);
        modem_loc_handle_rx(0x02u, 13u, 0x0020u, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);
        assert(rev_done && modem_loc_diag.loc_rev == 2u);
        tp = 0; svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 3u);
        modem_loc_handle_rx(0x04u, 0u, 0x0020u, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);

        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 1u, 48u);
        modem_loc_handle_rx(0x02u, 14u, 0x0021u, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);
        assert(!reg_ok && retry_min);

        retry_min = false;
        tp = 0; svc_tlv_put_result(tlvs, sizeof(tlvs), &tp, 0u, 0u);
        modem_loc_handle_rx(0x02u, 15u, 0x0021u, tlvs, (uint16_t)tp, &rev_done, &reg_ok, &retry_min);
        assert(reg_ok && !retry_min);

        tp = 0; svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 0u);
        svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x02u, 1u);
        svc_tlv_put_u64(tlvs, sizeof(tlvs), &tp, 0x10u, 0x4042c00000000000ULL);
        svc_tlv_put_u64(tlvs, sizeof(tlvs), &tp, 0x11u, 0x405e000000000000ULL);
        modem_loc_handle_rx(0x04u, 0u, 0x0024u, tlvs, (uint16_t)tp, NULL, &reg_ok, NULL);
        tp = 0; svc_tlv_put_u8(tlvs, sizeof(tlvs), &tp, 0x10u, 6u);
        modem_loc_handle_rx(0x04u, 0u, 0x0025u, tlvs, (uint16_t)tp, NULL, &reg_ok, NULL);
        tp = 0; svc_tlv_put(tlvs, sizeof(tlvs), &tp, 0x01u, "$GPGGA,123456.00,3723.2475,N,12158.3416,W", 41u);
        modem_loc_handle_rx(0x04u, 0u, 0x0026u, tlvs, (uint16_t)tp, NULL, &reg_ok, NULL);
        tp = 0; svc_tlv_put_u32(tlvs, sizeof(tlvs), &tp, 0x01u, 1u);
        modem_loc_handle_rx(0x04u, 0u, 0x002bu, tlvs, (uint16_t)tp, NULL, &reg_ok, NULL);
        modem_loc_handle_rx(0x04u, 0u, 0x002cu, tlvs, (uint16_t)tp, NULL, &reg_ok, NULL);

        char loc_st[128] = {{ 0 }};
        modem_format_loc_status(loc_st, sizeof(loc_st), reg_ok, true);
        assert(strcmp(loc_st, "dms_mode=3 dms_err=58 uim_card=1 uim_err=0 loc_reg=1 loc_fb=1 loc_err=0 loc_rev=3 loc_sv=6 loc_eng=1 loc_fix=1") == 0);
    }}

    for (unsigned i = 0; i < RMTFS_PART_COUNT; i++) {{
        free(rmtfs_parts[i].shadow);
        rmtfs_parts[i].shadow = NULL;
    }}
    lowdma_enabled = false;
}}

static size_t build_elf(uint8_t *out, uint16_t phnum, const struct ipa_elf32_phdr *phs) {{
    memset(out, 0, 52 + phnum * 32);
    memcpy(out, "\177ELF\1", 5);
    out[44] = (uint8_t)(phnum & 0xff);
    out[45] = (uint8_t)(phnum >> 8);
    memcpy(out + 52, phs, phnum * 32);
    return 52 + phnum * 32;
}}

int main(void) {{
    /* 1. Relocatable ipa_fws (min_paddr = 0, span = 0x5000), arginfo 0x82 succeeds cleanly. */
    static uint8_t mdt[512], b02[128], b03[256];
    for (size_t i = 0; i < sizeof(b02); i++) b02[i] = (uint8_t)(0x10 + i);
    for (size_t i = 0; i < sizeof(b03); i++) b03[i] = (uint8_t)(0x80 ^ i);
    struct ipa_elf32_phdr phs_reloc[4] = {{
        {{ .type = 6, .paddr = 0, .filesz = 0, .memsz = 0, .flags = 0 }},
        {{ .type = 1, .paddr = 0, .filesz = 32, .memsz = 32, .flags = (2u << 24) }}, /* HASH skip */
        {{ .type = 1, .paddr = 0x0, .filesz = sizeof(b02), .memsz = 0x1000, .flags = (1u << 27) }},
        {{ .type = 1, .paddr = 0x2000, .filesz = sizeof(b03), .memsz = 0x2100, .flags = (1u << 27) }},
    }};
    size_t mdt_sz = build_elf(mdt, 4, phs_reloc);
    mock_files.count = 3;
    strcpy(mock_files.names[0], "ipa_fws.mdt"); mock_files.data[0] = mdt; mock_files.sizes[0] = mdt_sz;
    strcpy(mock_files.names[1], "ipa_fws.b02"); mock_files.data[1] = b02; mock_files.sizes[1] = sizeof(b02);
    strcpy(mock_files.names[2], "ipa_fws.b03"); mock_files.data[2] = b03; mock_files.sizes[2] = sizeof(b03);

    reject_init_0x82 = false;
    uint32_t used_pas = 0;
    assert(annibale_load_mdt_image("ipa_fws", 15, 0, 0x9b080000ULL, 0x10000ULL, &used_pas));
    assert(used_pas == 15);
    assert(mem_setup_before_map == 1 && map_calls == 1 && unmap_calls == 1);
    assert(last_setup_size == 0x10000ULL);
    assert(cacheclean_calls >= 64); /* 64B dc civac across whole page */
    assert(smc_count == 2);
    assert(strcmp(smc_seq[0], "INIT:15:0x82") == 0);
    assert(strcmp(smc_seq[1], "SETUP:15:0x10000") == 0);
    assert(memcmp(mock_carveout + 0, b02, sizeof(b02)) == 0);
    for (size_t z = sizeof(b02); z < 0x1000; z++) assert(mock_carveout[z] == 0);
    assert(memcmp(mock_carveout + 0x2000, b03, sizeof(b03)) == 0);
    for (size_t z = 0x2000 + sizeof(b03); z < 0x4100; z++) assert(mock_carveout[z] == 0);

    /* 1b. When 0x82 fails, do NOT fall back to bogus 0x22; fail cleanly without mapping carveout. */
    reject_init_0x82 = true;
    smc_count = 0;
    map_calls = 0;
    assert(!annibale_load_mdt_image("ipa_fws", 15, 0, 0x9b080000ULL, 0x10000ULL, NULL));
    assert(smc_count == 1 && strcmp(smc_seq[0], "INIT:15:0x82") == 0);
    assert(map_calls == 0);
    reject_init_0x82 = false;

    /* 1c. With lowdma enabled, three distinct < 4 GiB slots (pas=38 slot 0, pas=4 slot 1, assign slot 2) with VM_WIMG_DEFAULT. */
    for (unsigned s = 0; s < IPA_TZ_HDR_SLOTS; s++) {{ ipa_tz_hdr_va[s] = 0; ipa_tz_hdr_pa[s] = 0; }}
    lowdma_enabled = true; lowdma_map_calls = 0; smc_count = 0; last_init_count = 0; map_calls = 0; unmap_calls = 0;
    assert(annibale_load_mdt_image("ipa_fws", 38, 0, 0x9b080000ULL, 0x10000ULL, &used_pas) && used_pas == 38);
    assert(lowdma_map_calls == 1 && lowdma_map_fl == VM_WIMG_DEFAULT);
    assert(annibale_load_mdt_image("ipa_fws", 4, 0, 0x9b080000ULL, 0x10000ULL, &used_pas) && used_pas == 4);
    assert(lowdma_map_calls == 1 && last_init_count == 2);
    assert(last_init_hdr_pas[0] == 0xcf6fc000ULL && last_init_hdr_pas[1] == 0xcf70c000ULL && ipa_tz_hdr_pa[2] == 0xcf71c000ULL);
    assert(smc_count == 4 && strcmp(smc_seq[0], "INIT:38:0x82") == 0 && strcmp(smc_seq[2], "INIT:4:0x82") == 0);
    assert(memcmp(mock_lowdma + 0, mdt, mdt_sz) == 0 && memcmp(mock_lowdma + IPA_TZ_HDR_SLOT_SZ, mdt, mdt_sz) == 0);

    /* 1d. annibale_tz_assign_mem uses slot 2 for BOTH exclusive (DSM 1&2, dst_sz=24) and shared (memshare/rmtfs, dst_sz=48). */
    const uint64_t slot2_pa = 0xcf71c000ULL;
    const uint8_t *slot2 = mock_lowdma + 2u * IPA_TZ_HDR_SLOT_SZ;
    struct {{ uint64_t pa, sz; bool shared; }} regions[3] = {{
        {{ 0x84a00000ULL, 0x04900000ULL, false }},
        {{ 0x89300000ULL, 0x00a80000ULL, false }},
        {{ 0x82800000ULL, 0x00400000ULL, true }},
    }};
    for (unsigned r = 0; r < 3; r++) {{
        uint64_t res1 = 0xdeadbeefULL;
        smc_count = 0; cacheclean_calls = 0; cacheclean_addr_count = 0; host_dsb_count = 0;
        mock_assign_rc = 0; mock_assign_res1 = 0;
        assert(annibale_tz_assign_mem(regions[r].pa, regions[r].sz, 15u, regions[r].shared, &res1) == 0 && res1 == 0);
        assert(smc_count == 1 && strcmp(smc_seq[0], "ASSIGN:0x1117") == 0);
        assert(last_assign_smc.fn == SCM_MP_ASSIGN && last_assign_smc.arginfo == 0x1117ULL);
        assert(last_assign_smc.a0 == slot2_pa + 0x040ULL && last_assign_smc.a1 == 16ULL);
        assert(last_assign_smc.a2 == slot2_pa + 0x000ULL && last_assign_smc.a3 == slot2_pa + 0x100ULL);
        assert(last_assign_smc.cacheclean_before_smc == 8u && last_assign_smc.dsb_before_smc == 1u);
        for (unsigned c = 0; c < 8u; c++) assert(cacheclean_addrs[c] == (uintptr_t)(slot2 + c * 64u));
        uint32_t src_v = 0; struct tz_assign_mem mem_v; struct tz_assign_perm dst_v[2]; uint64_t ext_v[7];
        memcpy(&src_v, slot2 + 0x000u, sizeof(src_v)); memcpy(&mem_v, slot2 + 0x040u, sizeof(mem_v));
        memcpy(dst_v, slot2 + 0x080u, sizeof(dst_v)); memcpy(ext_v, slot2 + 0x100u, sizeof(ext_v));
        assert(src_v == 3u && mem_v.addr == regions[r].pa && mem_v.size == regions[r].sz);
        if (regions[r].shared) {{
            assert(dst_v[0].vmid == 3u && dst_v[0].perm == 6u && !dst_v[0].ctx && !dst_v[0].ctx_size && !dst_v[0].pad);
            assert(dst_v[1].vmid == 15u && dst_v[1].perm == 6u && !dst_v[1].ctx && !dst_v[1].ctx_size && !dst_v[1].pad);
            assert(ext_v[0] == 4ULL && ext_v[1] == slot2_pa + 0x080ULL && ext_v[2] == 48ULL);
        }} else {{
            assert(dst_v[0].vmid == 15u && dst_v[0].perm == 6u && !dst_v[0].ctx && !dst_v[0].ctx_size && !dst_v[0].pad);
            assert(!dst_v[1].vmid && !dst_v[1].perm && !dst_v[1].ctx && !dst_v[1].ctx_size && !dst_v[1].pad);
            assert(ext_v[0] == 4ULL && ext_v[1] == slot2_pa + 0x080ULL && ext_v[2] == 24ULL);
        }}
        assert(!ext_v[3] && !ext_v[4] && !ext_v[5] && !ext_v[6]);
        for (unsigned z = 0x138u; z < 0x200u; z++) assert(slot2[z] == 0);
    }}
    /* Slot 0 & slot 1 headers remain untouched after slot 2 assign calls. */
    assert(memcmp(mock_lowdma + 0, mdt, mdt_sz) == 0);
    assert(memcmp(mock_lowdma + IPA_TZ_HDR_SLOT_SZ, mdt, mdt_sz) == 0);
    /* Error propagation & zero-size guard. */
    {{
        uint64_t res1 = 0;
        mock_assign_rc = -2; mock_assign_res1 = 0x1234ULL;
        assert(annibale_tz_assign_mem(0x84a00000ULL, 0x1000ULL, 15u, false, &res1) == -2);
        assert(res1 == 0x1234ULL);
        mock_assign_rc = 0; mock_assign_res1 = 0; res1 = 0x5555ULL;
        assert(annibale_tz_assign_mem(0x84a00000ULL, 0ULL, 15u, false, &res1) == -1 && res1 == 0);
    }}
    lowdma_enabled = false;

    /* 2. Non-relocatable ELF with out-of-range paddr must fail cleanly without touching carveout. */
    struct ipa_elf32_phdr phs_bad[2] = {{
        {{ .type = 6, .paddr = 0, .filesz = 0, .memsz = 0, .flags = 0 }},
        {{ .type = 1, .paddr = 0x10000000u, .filesz = 0, .memsz = 0x1000, .flags = 0 }},
    }};
    mock_files.sizes[0] = build_elf(mdt, 2, phs_bad);
    map_calls = 0;
    assert(!annibale_load_mdt_image("ipa_fws", 15, 0, 0x9b080000ULL, 0x10000ULL, NULL));
    assert(map_calls == 0);

    /* 3. modem=0 does NOT launch modem_worker_thread. */
    annibale_modem_init();
    assert(modem_inited && !worker_started);

    /* 4. SMEM 421 crash reason sanitizes any >=5-digit identifier runs while preserving short error codes. */
    char sanitized[128];
    const char *raw = "err_fatal file.c:412 imei=123456789012345 sub=987654 line=42";
    modem_sanitize_crash_str(raw, (uint32_t)strlen(raw), sanitized, sizeof(sanitized));
    assert(strcmp(sanitized, "err_fatal file.c:412 imei=*** sub=*** line=42") == 0);

    /* 5. SMEM GLINK handshake, desc offsets, intents, reassembly & lock ordering. */
    test_glink_mpss();

    /* 6. APPS-side QMI servers (RMTFS 0x0e, MEMSHARE 0x34, SERVREG_LOCATOR 0x40) & QMI_LOC. */
    test_modem_qmi_services();

    printf("ok  test-annibale-modem-loader (relocatable ELF, PAS order, HYP_ASSIGN, modem=0 gate, SMEM 421, GLINK IPCRTR, RMTFS/MEMSHARE/SERVREG/LOC)\n");
    return 0;
}}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(harness)
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(src), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)

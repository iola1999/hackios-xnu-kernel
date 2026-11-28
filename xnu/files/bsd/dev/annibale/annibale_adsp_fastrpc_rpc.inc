#ifndef _ANNIBALE_ADSP_FASTRPC_RPC_INC_
#define _ANNIBALE_ADSP_FASTRPC_RPC_INC_

/*
 * FastRPC wire ABI, adsp_mem_heap (12 MiB <4 GiB), and apps_std / apps_remotectl reverse RPC.
 *
 * Matches Linux drivers/misc/fastrpc.c and Qualcomm apps_std.idl / adsp_listener.idl:
 *   - struct fastrpc_msg (40 bytes) & struct fastrpc_invoke_rsp (16 bytes)
 *   - FASTRPC_INIT_HANDLE (1), _const_adsp_listener_handle (3)
 *   - FASTRPC_RMID_INIT_CREATE_STATIC (8), FASTRPC_RMID_INIT_ATTACH (0)
 *   - adsp_listener_init2 (mid 3), adsp_listener_next2 (mid 4), adsp_listener_get_in_bufs2 (mid 5)
 *   - Sensor file resolution aligned with /vendor/etc/sensors/config/ and
 *     /mnt/vendor/persist/sensors/registry/, logging missing files and returning ENOENT.
 */

#define ADSP_FASTRPC_ALIGN          128u
#define ADSP_FASTRPC_PAGE_SIZE      4096u
#define ADSP_FASTRPC_PAGE_MASK      4095ULL
#define ADSP_FASTRPC_FDLIST_MAX     16u
#define ADSP_FASTRPC_CRCLIST_MAX    64u

#define ADSP_FASTRPC_INIT_HANDLE    1u
#define ADSP_FASTRPC_LISTENER_HANDLE 3u
#define ADSP_FASTRPC_REV_REMOTECTL  0u
#define ADSP_FASTRPC_REV_APPS_STD   0x10u
#define ADSP_FASTRPC_REV_ADSPMSGD   0x11u

#define ADSP_FASTRPC_RMID_INIT_ATTACH        0u
#define ADSP_FASTRPC_RMID_INIT_CREATE_STATIC 8u
#define ADSP_FASTRPC_RMID_INIT_ATTACH_SNS    10u
#define ADSP_FASTRPC_LISTENER_INIT2          3u
#define ADSP_FASTRPC_LISTENER_NEXT2          4u
#define ADSP_FASTRPC_LISTENER_GET_IN_BUFS2   5u

#define ADSP_FASTRPC_ROOT_PD        0u
#define ADSP_FASTRPC_USER_PD        1u
#define ADSP_FASTRPC_SENSORS_PD     2u

#define ADSP_FASTRPC_HEAP_SIZE      0x00c00000u /* 12 MiB, annibale.dts adsp_mem_heap */
#define ADSP_FASTRPC_INVOKE_BUF_SZ  0x00010000u /* 64 KiB invoke + reverse-RPC staging */
#define ADSP_FASTRPC_IO_MAX_BYTES   (ADSP_FASTRPC_INVOKE_BUF_SZ / 2u)
#define ADSP_FASTRPC_VMID_LPASS     0x16u
#define ADSP_FASTRPC_VMID_ADSP_HEAP 0x25u
#define ADSP_FASTRPC_PERM_RW        6u
#define ADSP_FASTRPC_PERM_RWX       7u

#define ADSP_FASTRPC_SCALARS(method, in, out) \
	((((uint32_t)(method) & 0x1fu) << 24) | (((uint32_t)(in) & 0xffu) << 16) | (((uint32_t)(out) & 0xffu) << 8))
#define ADSP_SC_METHOD(sc)          (((sc) >> 24) & 0x1fu)
#define ADSP_SC_INBUFS(sc)          (((sc) >> 16) & 0xffu)
#define ADSP_SC_OUTBUFS(sc)         (((sc) >> 8) & 0xffu)
#define ADSP_SC_INHANDLES(sc)       (((sc) >> 4) & 0x0fu)
#define ADSP_SC_OUTHANDLES(sc)      ((sc) & 0x0fu)

#define ADSP_FASTRPC_MAX_FDS        16u
#define ADSP_FASTRPC_FD_BASE        100
#define ADSP_FASTRPC_MISSING_LOGS   32u
#define ADSP_FASTRPC_MAX_ARGS       16u

#define APPS_STD_OP_FOPEN           0u
#define APPS_STD_OP_FFLUSH          2u
#define APPS_STD_OP_FCLOSE          3u
#define APPS_STD_OP_FREAD           4u
#define APPS_STD_OP_FTELL           8u
#define APPS_STD_OP_FSEEK           9u
#define APPS_STD_OP_FLEN            10u
#define APPS_STD_OP_REWIND          11u
#define APPS_STD_OP_FEOF            12u
#define APPS_STD_OP_FERROR          13u
#define APPS_STD_OP_CLEARERR        14u
#define APPS_STD_OP_PRINT_STRING    15u
#define APPS_STD_OP_GETENV          16u
#define APPS_STD_OP_FOPEN_WITH_ENV  19u
#define APPS_STD_OP_FILE_EXISTS     22u
#define APPS_STD_OP_OPENDIR         26u
#define APPS_STD_OP_CLOSEDIR        27u
#define APPS_STD_OP_EXT             31u
#define APPS_STD_EXT_STAT           31u

struct fastrpc_wire_msg {
	int32_t  client_id;
	int32_t  tid;
	uint64_t ctx;
	uint32_t handle;
	uint32_t sc;
	uint64_t addr;
	uint64_t size;
};

struct fastrpc_wire_rsp {
	uint64_t ctx;
	int32_t  retval;
	uint32_t pad;
};

struct fastrpc_remote_buf {
	uint64_t pv;
	uint64_t len;
};

struct fastrpc_invoke_buf {
	uint32_t num;
	uint32_t pgidx;
};

struct fastrpc_phy_page {
	uint64_t addr;
	uint64_t size;
};

struct fastrpc_init_create_static_inbuf {
	int32_t  client_id;
	uint32_t namelen;
	uint32_t pageslen;
};

struct apps_std_stat_wire {
	uint64_t tsz;
	uint64_t dev;
	uint64_t ino;
	uint32_t mode;
	uint32_t nlink;
	uint64_t rdev;
	uint64_t size;
	int64_t  atime;
	int64_t  atimensec;
	int64_t  mtime;
	int64_t  mtimensec;
	int64_t  ctime;
	int64_t  ctimensec;
};

_Static_assert(sizeof(struct fastrpc_wire_msg) == 40, "fastrpc_wire_msg size");
_Static_assert(sizeof(struct fastrpc_wire_rsp) == 16, "fastrpc_wire_rsp size");
_Static_assert(sizeof(struct apps_std_stat_wire) == 96, "apps_std_stat_wire size");

struct adsp_fastrpc_host_arg {
	const void *in_data;
	void       *out_ptr;
	uint32_t    len;
};

struct adsp_fastrpc_rev_arg {
	uint8_t *pv;
	uint32_t len;
};

struct adsp_fastrpc_fd {
	bool           used;
	bool           owned;
	bool           eof;
	const uint8_t *data;
	uint64_t       size;
	uint64_t       pos;
};

static struct adsp_fastrpc_fd adsp_fastrpc_fds[ADSP_FASTRPC_MAX_FDS];
static uint32_t adsp_fastrpc_missing_logs;
static uint32_t adsp_fastrpc_open_logs;

static uint64_t adsp_fastrpc_heap_va;
static uint64_t adsp_fastrpc_heap_pa;
static bool adsp_fastrpc_heap_ready;

static uint64_t adsp_fastrpc_buf_va;
static uint64_t adsp_fastrpc_buf_pa;
static bool adsp_fastrpc_buf_ready;
static bool adsp_fastrpc_buf_from_lowdma;

/*
 * Carve adsp_mem_heap (12 MiB, 4 MiB-aligned) and invoke staging buffer (64 KiB)
 * from the <4 GiB loader lowdma window, strictly above ath12k/MHI (80 MiB lx +
 * firmware/rings) and strictly below modem_svc (rmtfs + memshare), audio, and IPA.
 */
static bool
adsp_fastrpc_lowdma_pick(uint64_t low, uint64_t mb,
    uint64_t *heap_pa_out, uint64_t *buf_pa_out)
{
	if (low == 0 || mb < 96 || mb > 1024) {
		return false;
	}
	uint64_t low_end = low + (mb << 20);
	if (low_end > 0x100000000ULL || low_end < low + 0x06d00000ULL) {
		return false;
	}
	uint64_t wifi_ceiling = (low + 0x06000000ULL + 0xfffffULL) & ~0xfffffULL;
	uint64_t top_limit = (low_end - 0x0012c000ULL) & ~0xfffffULL;
	uint64_t modem_safe = (low + 0x070c0000ULL + 0xfffffULL) & ~0xfffffULL;
	uint64_t modem_min = top_limit > 0x00e00000ULL ? (top_limit - 0x00e00000ULL) : 0ULL;

	if (modem_safe < modem_min) {
		modem_safe = modem_min;
	}
	if (modem_safe < top_limit) {
		top_limit = modem_safe;
	}
	if (top_limit < ADSP_FASTRPC_HEAP_SIZE + ADSP_FASTRPC_INVOKE_BUF_SZ) {
		return false;
	}
	uint64_t heap_pa = (top_limit - ADSP_FASTRPC_HEAP_SIZE) & ~0x3fffffULL;
	uint64_t buf_pa = (heap_pa + ADSP_FASTRPC_HEAP_SIZE + ADSP_FASTRPC_INVOKE_BUF_SZ <= top_limit)
	    ? (heap_pa + ADSP_FASTRPC_HEAP_SIZE)
	    : (heap_pa - ADSP_FASTRPC_INVOKE_BUF_SZ);
	uint64_t region_min = buf_pa < heap_pa ? buf_pa : heap_pa;

	if (heap_pa < wifi_ceiling || region_min < wifi_ceiling ||
	    heap_pa + ADSP_FASTRPC_HEAP_SIZE > top_limit ||
	    buf_pa + ADSP_FASTRPC_INVOKE_BUF_SZ > top_limit) {
		return false;
	}
	*heap_pa_out = heap_pa;
	*buf_pa_out = buf_pa;
	return true;
}

#ifndef ADSP_FASTRPC_HOST_TEST
extern int64_t annibale_tz_assign_mem_perms(uint64_t region_pa, uint64_t region_sz,
    const uint32_t *dst_vmids, const uint32_t *dst_perms, uint32_t dst_cnt, uint64_t *res1_out);

static void
adsp_fastrpc_cache_clean(uint64_t va, uint32_t size)
{
	uint64_t end = va + size;
	for (uint64_t p = va & ~63ULL; p < end; p += 64ULL) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	__builtin_arm_dsb(0xf);
}

static bool
adsp_fastrpc_alloc_contig_low4g(uint32_t size, uint32_t align_mask, uint64_t *va_out, uint64_t *pa_out)
{
	vm_offset_t kva = 0;
	ppnum_t max_pnum = (ppnum_t)(0xffffffffULL >> PAGE_SHIFT);

	if (kmem_alloc_contig(kernel_map, &kva, size, align_mask, max_pnum, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		if (align_mask != PAGE_MASK &&
		    kmem_alloc_contig(kernel_map, &kva, size, PAGE_MASK, max_pnum, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
	}
	uint64_t base_pa = kvtophys(kva);
	if (base_pa == 0 || base_pa + size > 0x100000000ULL) {
		kmem_free(kernel_map, kva, size);
		return false;
	}
	for (uint32_t off = PAGE_SIZE; off < size; off += PAGE_SIZE) {
		if (kvtophys(kva + off) != base_pa + off) {
			kmem_free(kernel_map, kva, size);
			return false;
		}
	}
	adsp_fastrpc_cache_clean((uint64_t)kva, size);
	*va_out = (uint64_t)kva;
	*pa_out = base_pa;
	return true;
}

static bool adsp_fastrpc_assign_heap(uint64_t pa);

static bool
adsp_fastrpc_ensure_mem(void)
{
	uint64_t low = 0, mb = 0;
	uint64_t low_heap_pa = 0, low_buf_pa = 0;
	bool has_lowdma = PE_parse_boot_argn("lowdma", &low, sizeof(low)) &&
	    PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) &&
	    adsp_fastrpc_lowdma_pick(low, mb, &low_heap_pa, &low_buf_pa);

	if (!adsp_fastrpc_heap_ready) {
		uint64_t va = 0, pa = 0;
		if (has_lowdma) {
			vm_offset_t kva = ml_io_map_unmappable((vm_offset_t)low_heap_pa,
			    ADSP_FASTRPC_HEAP_SIZE, VM_WIMG_DEFAULT);
			if (kva != 0) {
				bzero((void *)kva, ADSP_FASTRPC_HEAP_SIZE);
				adsp_fastrpc_cache_clean((uint64_t)kva, ADSP_FASTRPC_HEAP_SIZE);
				ml_io_unmap(kva, ADSP_FASTRPC_HEAP_SIZE);
				pa = low_heap_pa;
			}
		}
		if (pa == 0 &&
		    !adsp_fastrpc_alloc_contig_low4g(ADSP_FASTRPC_HEAP_SIZE, 0x3fffffULL, &va, &pa)) {
			say("fastrpc: adsp_mem_heap 12 MiB <4G allocation failed\n");
			return false;
		}
		(void)adsp_fastrpc_assign_heap(pa);
		adsp_fastrpc_heap_va = va;
		adsp_fastrpc_heap_pa = pa;
		adsp_fastrpc_heap_ready = true;
	}
	if (!adsp_fastrpc_buf_ready) {
		uint64_t va = 0, pa = 0;
		bool from_lowdma = false;
		if (has_lowdma) {
			vm_offset_t kva = ml_io_map_unmappable((vm_offset_t)low_buf_pa,
			    ADSP_FASTRPC_INVOKE_BUF_SZ, VM_WIMG_DEFAULT);
			if (kva != 0) {
				bzero((void *)kva, ADSP_FASTRPC_INVOKE_BUF_SZ);
				adsp_fastrpc_cache_clean((uint64_t)kva, ADSP_FASTRPC_INVOKE_BUF_SZ);
				va = (uint64_t)kva;
				pa = low_buf_pa;
				from_lowdma = true;
			}
		}
		if (!from_lowdma &&
		    !adsp_fastrpc_alloc_contig_low4g(ADSP_FASTRPC_INVOKE_BUF_SZ, PAGE_MASK, &va, &pa)) {
			say("fastrpc: invoke buffer %u B allocation failed\n", ADSP_FASTRPC_INVOKE_BUF_SZ);
			return false;
		}
		adsp_fastrpc_buf_va = va;
		adsp_fastrpc_buf_pa = pa;
		adsp_fastrpc_buf_from_lowdma = from_lowdma;
		adsp_fastrpc_buf_ready = true;
	}
	return true;
}

static void
adsp_fastrpc_free_buf(void)
{
	if (adsp_fastrpc_buf_ready && adsp_fastrpc_buf_va != 0) {
		if (adsp_fastrpc_buf_from_lowdma) {
			ml_io_unmap((vm_offset_t)adsp_fastrpc_buf_va, ADSP_FASTRPC_INVOKE_BUF_SZ);
		} else {
			kmem_free(kernel_map, (vm_offset_t)adsp_fastrpc_buf_va, ADSP_FASTRPC_INVOKE_BUF_SZ);
		}
	}
	adsp_fastrpc_buf_va = 0;
	adsp_fastrpc_buf_pa = 0;
	adsp_fastrpc_buf_from_lowdma = false;
	adsp_fastrpc_buf_ready = false;
}

static bool
adsp_fastrpc_try_vfs(const char *path, const uint8_t **data_out, uint64_t *size_out, bool *owned_out)
{
	if (rootvnode == NULLVP || path == NULL || path[0] != '/') {
		return false;
	}
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	if (vnode_lookup(path, 0, &vp, ctx) != 0) {
		return false;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_type);
	if (vnode_getattr(vp, &va, ctx) != 0 || va.va_type != VREG || va.va_data_size > 0x01000000ULL) {
		vnode_put(vp);
		return false;
	}
	if (va.va_data_size == 0) {
		vnode_put(vp);
		*data_out = (const uint8_t *)"";
		*size_out = 0;
		*owned_out = false;
		return true;
	}
	uint8_t *buf = (uint8_t *)kalloc_data((size_t)va.va_data_size, Z_WAITOK);
	if (buf == NULL) {
		vnode_put(vp);
		return false;
	}
	int resid = 0;
	int err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)va.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_put(vp);
	if (err != 0 || resid != 0) {
		kfree_data(buf, (size_t)va.va_data_size);
		return false;
	}
	*data_out = buf;
	*size_out = va.va_data_size;
	*owned_out = true;
	return true;
}

static bool
adsp_fastrpc_try_fw(const char *rel, const uint8_t **data_out, uint64_t *size_out, bool *owned_out)
{
	const uint8_t *img = annibale_image_file("fw", rel, size_out, NULL);
	if (img != NULL) {
		*data_out = img;
		*owned_out = false;
		return true;
	}
	return false;
}

static void
adsp_fastrpc_free_fd_data(struct adsp_fastrpc_fd *slot)
{
	if (slot->owned && slot->data != NULL && slot->size > 0) {
		void *p = __DECONST(void *, slot->data);
		kfree_data(p, (size_t)slot->size);
	}
}
#endif

static bool
adsp_fastrpc_assign_heap(uint64_t pa)
{
	/* Match annibale DTS adsp_heap_region (qcom,vmids = <0x16 0x25>) and upstream Linux fastrpc (RWX=7). */
	static const struct {
		uint32_t vmids[2];
		uint32_t perms[2];
		uint32_t cnt;
	} candidates[] = {
		{ { ADSP_FASTRPC_VMID_LPASS, ADSP_FASTRPC_VMID_ADSP_HEAP }, { ADSP_FASTRPC_PERM_RWX, ADSP_FASTRPC_PERM_RWX }, 2u },
		{ { ADSP_FASTRPC_VMID_LPASS, ADSP_FASTRPC_VMID_ADSP_HEAP }, { ADSP_FASTRPC_PERM_RW, ADSP_FASTRPC_PERM_RW }, 2u },
		{ { ADSP_FASTRPC_VMID_ADSP_HEAP, 0u }, { ADSP_FASTRPC_PERM_RWX, 0u }, 1u },
		{ { ADSP_FASTRPC_VMID_LPASS, 0u }, { ADSP_FASTRPC_PERM_RWX, 0u }, 1u },
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		uint64_t res1 = 0;
		int64_t rc = annibale_tz_assign_mem_perms(pa, ADSP_FASTRPC_HEAP_SIZE,
		    candidates[i].vmids, candidates[i].perms, candidates[i].cnt, &res1);
		say("fastrpc: adsp_mem_heap pa=%#llx sz=%#x cnt=%u vmids=%#x,%#x perm=%u hyp_assign=(%lld,%#llx)\n",
		    pa, ADSP_FASTRPC_HEAP_SIZE, candidates[i].cnt, candidates[i].vmids[0],
		    candidates[i].vmids[1], candidates[i].perms[0], rc, res1);
		if (rc == 0 && res1 == 0) {
			return true;
		}
	}
	return false;
}

static uint32_t
adsp_fastrpc_align_up(uint32_t v, uint32_t align)
{
	return (v + align - 1u) & ~(align - 1u);
}

static bool
adsp_fastrpc_build_invoke(uint32_t sc, const struct adsp_fastrpc_host_arg *args,
    uint32_t *out_offsets, uint32_t *pkt_size_out)
{
	uint32_t nscalars = ADSP_SC_INBUFS(sc) + ADSP_SC_OUTBUFS(sc) +
	    ADSP_SC_INHANDLES(sc) + ADSP_SC_OUTHANDLES(sc);
	uint32_t nbufs = ADSP_SC_INBUFS(sc) + ADSP_SC_OUTBUFS(sc);
	if (!adsp_fastrpc_buf_ready || nscalars > ADSP_FASTRPC_MAX_ARGS || nbufs > nscalars) {
		return false;
	}

	uint32_t metalen = nscalars * (uint32_t)(sizeof(struct fastrpc_remote_buf) +
	    sizeof(struct fastrpc_invoke_buf) + sizeof(struct fastrpc_phy_page)) +
	    ADSP_FASTRPC_FDLIST_MAX * 8u + ADSP_FASTRPC_CRCLIST_MAX * 4u + 4u;
	uint32_t cursor = adsp_fastrpc_align_up(metalen, ADSP_FASTRPC_ALIGN);
	uint32_t total_sz = cursor;
	for (uint32_t i = 0; i < nbufs; i++) {
		if (args[i].len > ADSP_FASTRPC_INVOKE_BUF_SZ) {
			return false;
		}
		if (args[i].len > 0) {
			total_sz = adsp_fastrpc_align_up(total_sz, ADSP_FASTRPC_ALIGN);
			if (total_sz + args[i].len > ADSP_FASTRPC_INVOKE_BUF_SZ) {
				return false;
			}
			total_sz += args[i].len;
		}
	}
	uint32_t aligned_pkt = adsp_fastrpc_align_up(total_sz, ADSP_FASTRPC_PAGE_SIZE);
	if (aligned_pkt > ADSP_FASTRPC_INVOKE_BUF_SZ) {
		return false;
	}

	uint8_t *base = (uint8_t *)(uintptr_t)adsp_fastrpc_buf_va;
	memset(base, 0, aligned_pkt);

	struct fastrpc_remote_buf *rpra = (struct fastrpc_remote_buf *)(uintptr_t)adsp_fastrpc_buf_va;
	struct fastrpc_invoke_buf *list = (struct fastrpc_invoke_buf *)(void *)(rpra + nscalars);
	struct fastrpc_phy_page *pages = (struct fastrpc_phy_page *)(void *)(list + nscalars);
	uint64_t sid_hi = (uint64_t)ADSP_FASTRPC_COMPUTE_SID << 32;

	for (uint32_t i = 0; i < nbufs; i++) {
		uint32_t len = args[i].len;
		rpra[i].len = len;
		list[i].num = len > 0 ? 1u : 0u;
		list[i].pgidx = i;
		if (out_offsets != NULL) {
			out_offsets[i] = 0;
		}
		if (len == 0) {
			continue;
		}
		cursor = adsp_fastrpc_align_up(cursor, ADSP_FASTRPC_ALIGN);
		uint64_t dma = sid_hi | (adsp_fastrpc_buf_pa + cursor);
		rpra[i].pv = dma;
		pages[i].addr = dma & ~ADSP_FASTRPC_PAGE_MASK;
		pages[i].size = (((dma & ADSP_FASTRPC_PAGE_MASK) + len + ADSP_FASTRPC_PAGE_MASK) &
		    ~ADSP_FASTRPC_PAGE_MASK);
		if (i < ADSP_SC_INBUFS(sc) && args[i].in_data != NULL) {
			memcpy(base + cursor, args[i].in_data, len);
		}
		if (out_offsets != NULL) {
			out_offsets[i] = cursor;
		}
		cursor += len;
	}

	adsp_fastrpc_cache_clean(adsp_fastrpc_buf_va, aligned_pkt);
	*pkt_size_out = total_sz;
	return true;
}

static void
adsp_fastrpc_close_all_fds(void)
{
	for (uint32_t i = 0; i < ADSP_FASTRPC_MAX_FDS; i++) {
		if (adsp_fastrpc_fds[i].used) {
			adsp_fastrpc_free_fd_data(&adsp_fastrpc_fds[i]);
			memset(&adsp_fastrpc_fds[i], 0, sizeof(adsp_fastrpc_fds[i]));
		}
	}
}

static bool
adsp_fastrpc_try_sensor_candidates(const char *rel, const uint8_t **data_out, uint64_t *size_out, bool *owned_out)
{
	static const char *const fw_prefixes[] = {
		"", "sensors/", "sensors/registry/", "sensors/registry/registry/",
	};
	static const char *const vfs_prefixes[] = {
		"/vendor/etc/sensors/config/", "/mnt/vendor/persist/sensors/registry/",
		"/mnt/vendor/persist/sensors/registry/registry/", "/vendor/etc/sensors/",
		"/fw/sensors/", "/fw/sensors/registry/", "/fw/sensors/registry/registry/",
		"/annibale/fw/sensors/", "/annibale/fw/sensors/registry/",
		"/annibale/fw/sensors/registry/registry/",
	};
	char cand[256];

	for (uint32_t i = 0; i < sizeof(fw_prefixes) / sizeof(fw_prefixes[0]); i++) {
		int n = snprintf(cand, sizeof(cand), "%s%s", fw_prefixes[i], rel);
		if (n > 0 && (size_t)n < sizeof(cand) && adsp_fastrpc_try_fw(cand, data_out, size_out, owned_out)) {
			return true;
		}
	}
	for (uint32_t i = 0; i < sizeof(vfs_prefixes) / sizeof(vfs_prefixes[0]); i++) {
		int n = snprintf(cand, sizeof(cand), "%s%s", vfs_prefixes[i], rel);
		if (n > 0 && (size_t)n < sizeof(cand) && adsp_fastrpc_try_vfs(cand, data_out, size_out, owned_out)) {
			return true;
		}
	}
	return false;
}

static bool
adsp_fastrpc_lookup_sensor_file(const char *req_name, const uint8_t **data_out, uint64_t *size_out, bool *owned_out)
{
	if (req_name == NULL || req_name[0] == '\0') {
		return false;
	}
	if (req_name[0] == '/' && adsp_fastrpc_try_vfs(req_name, data_out, size_out, owned_out)) {
		return true;
	}

	const char *rel = req_name;
	static const char *const strip_prefixes[] = {
		"/vendor/etc/sensors/config/",
		"/mnt/vendor/persist/sensors/registry/",
		"/vendor/etc/sensors/",
		"/persist/sensors/registry/",
	};
	for (uint32_t i = 0; i < sizeof(strip_prefixes) / sizeof(strip_prefixes[0]); i++) {
		size_t plen = strlen(strip_prefixes[i]);
		if (strncmp(req_name, strip_prefixes[i], plen) == 0) {
			rel = req_name + plen;
			break;
		}
	}
	if (rel[0] == '/') {
		rel++;
	}
	return adsp_fastrpc_try_sensor_candidates(rel, data_out, size_out, owned_out);
}

static void
adsp_fastrpc_log_missing(const char *op, const char *path)
{
	if (adsp_fastrpc_missing_logs < ADSP_FASTRPC_MISSING_LOGS) {
		adsp_fastrpc_missing_logs++;
		say("fastrpc: apps_std %s missing '%s' -> ENOENT\n", op, path ? path : "(null)");
	}
}

static int32_t
adsp_fastrpc_fd_alloc(const uint8_t *data, uint64_t size, bool owned)
{
	for (uint32_t i = 0; i < ADSP_FASTRPC_MAX_FDS; i++) {
		if (!adsp_fastrpc_fds[i].used) {
			adsp_fastrpc_fds[i].used = true;
			adsp_fastrpc_fds[i].owned = owned;
			adsp_fastrpc_fds[i].eof = false;
			adsp_fastrpc_fds[i].data = data;
			adsp_fastrpc_fds[i].size = size;
			adsp_fastrpc_fds[i].pos = 0;
			return (int32_t)(ADSP_FASTRPC_FD_BASE + i);
		}
	}
	return -1;
}

static struct adsp_fastrpc_fd *
adsp_fastrpc_fd_get(int32_t fd)
{
	if (fd < ADSP_FASTRPC_FD_BASE || fd >= (int32_t)(ADSP_FASTRPC_FD_BASE + ADSP_FASTRPC_MAX_FDS)) {
		return NULL;
	}
	struct adsp_fastrpc_fd *slot = &adsp_fastrpc_fds[fd - ADSP_FASTRPC_FD_BASE];
	return slot->used ? slot : NULL;
}

static bool
adsp_sbuf_unpack(const uint8_t *inbufs, uint32_t in_len, uint32_t sc, struct adsp_fastrpc_rev_arg *pra,
    uint32_t *out_req_sz)
{
	uint32_t nin = ADSP_SC_INBUFS(sc);
	uint32_t nout = ADSP_SC_OUTBUFS(sc);
	if (nin + nout > ADSP_FASTRPC_MAX_ARGS || ADSP_SC_INHANDLES(sc) != 0 || ADSP_SC_OUTHANDLES(sc) != 0) {
		return false;
	}
	uint32_t pos = 0;
	for (uint32_t i = 0; i < nin; i++) {
		if (pos + 4u > in_len) {
			return false;
		}
		uint32_t len = 0;
		memcpy(&len, inbufs + pos, 4u);
		pos += 4u;
		pra[i].len = len;
		pra[i].pv = NULL;
		if (len > 0) {
			pos = (pos + 7u) & ~7u;
			if (pos > in_len || len > in_len - pos) {
				return false;
			}
			pra[i].pv = (uint8_t *)(uintptr_t)(inbufs + pos);
			pos += len;
		}
	}
	uint32_t out_total = 0;
	for (uint32_t i = 0; i < nout; i++) {
		if (pos + 4u > in_len) {
			return false;
		}
		uint32_t len = 0;
		memcpy(&len, inbufs + pos, 4u);
		pos += 4u;
		if (len > ADSP_FASTRPC_IO_MAX_BYTES) {
			return false;
		}
		pra[nin + i].len = len;
		pra[nin + i].pv = NULL;
		out_total += 4u;
		if (len > 0) {
			out_total = ((out_total + 7u) & ~7u) + len;
			if (out_total > ADSP_FASTRPC_IO_MAX_BYTES) {
				return false;
			}
		}
	}
	*out_req_sz = out_total;
	return true;
}

static bool
adsp_sbuf_bind_outputs(uint8_t *outbufs, uint32_t out_cap, uint32_t nin, uint32_t nout,
    struct adsp_fastrpc_rev_arg *pra, uint32_t *out_len_used)
{
	uint32_t pos = 0;
	for (uint32_t i = 0; i < nout; i++) {
		uint32_t len = pra[nin + i].len;
		if (pos + 4u > out_cap) {
			return false;
		}
		memcpy(outbufs + pos, &len, 4u);
		pos += 4u;
		pra[nin + i].pv = NULL;
		if (len > 0) {
			pos = (pos + 7u) & ~7u;
			if (pos > out_cap || len > out_cap - pos) {
				return false;
			}
			pra[nin + i].pv = outbufs + pos;
			memset(pra[nin + i].pv, 0, len);
			pos += len;
		}
	}
	*out_len_used = pos;
	return true;
}

static bool
adsp_sbuf_str_ok(const struct adsp_fastrpc_rev_arg *arg, uint32_t expected_len)
{
	return expected_len > 0 && expected_len <= arg->len && arg->pv != NULL &&
	    arg->pv[expected_len - 1u] == '\0';
}

static int32_t
adsp_fastrpc_handle_apps_std(uint32_t sc, struct adsp_fastrpc_rev_arg *pra)
{
	uint32_t nin = ADSP_SC_INBUFS(sc);
	uint32_t nout = ADSP_SC_OUTBUFS(sc);
	uint32_t op = ADSP_SC_METHOD(sc);

	if (op == APPS_STD_OP_FOPEN || op == APPS_STD_OP_FOPEN_WITH_ENV) {
		bool with_env = (op == APPS_STD_OP_FOPEN_WITH_ENV);
		uint32_t req_in = with_env ? 5u : 3u;
		if (nin != req_in || nout != 1u || pra[0].len < req_in * 4u - 4u || pra[nin].len < 4u) {
			return EINVAL;
		}
		uint32_t lens[4] = { 0, 0, 0, 0 };
		memcpy(lens, pra[0].pv, (req_in - 1u) * 4u);
		uint32_t name_idx = with_env ? 3u : 1u;
		uint32_t mode_idx = with_env ? 4u : 2u;
		if (!adsp_sbuf_str_ok(&pra[name_idx], lens[name_idx - 1u]) ||
		    !adsp_sbuf_str_ok(&pra[mode_idx], lens[mode_idx - 1u])) {
			return EINVAL;
		}
		const char *path = (const char *)pra[name_idx].pv;
		const char *mode = (const char *)pra[mode_idx].pv;
		if (mode[0] != 'r') {
			adsp_fastrpc_log_missing("fopen(rw)", path);
			return ENOENT;
		}
		const uint8_t *data = NULL;
		uint64_t sz = 0;
		bool owned = false;
		if (!adsp_fastrpc_lookup_sensor_file(path, &data, &sz, &owned)) {
			adsp_fastrpc_log_missing(with_env ? "fopen_env" : "fopen", path);
			return ENOENT;
		}
		int32_t fd = adsp_fastrpc_fd_alloc(data, sz, owned);
		if (fd < 0) {
			if (owned && data != NULL && sz > 0) {
				struct adsp_fastrpc_fd tmp = { .owned = true, .data = data, .size = sz };
				adsp_fastrpc_free_fd_data(&tmp);
			}
			return ENOMEM;
		}
		memcpy(pra[nin].pv, &fd, 4u);
		if (adsp_fastrpc_open_logs < ADSP_FASTRPC_MISSING_LOGS) {
			adsp_fastrpc_open_logs++;
			say("fastrpc: apps_std opened '%s' size=%llu fd=%d\n", path, sz, fd);
		}
		return 0;
	}

	if (op == APPS_STD_OP_FCLOSE || op == APPS_STD_OP_FFLUSH || op == APPS_STD_OP_REWIND ||
	    op == APPS_STD_OP_CLEARERR) {
		if (nin != 1u || nout != 0u || pra[0].len < 4u) {
			return EINVAL;
		}
		int32_t fd = 0;
		memcpy(&fd, pra[0].pv, 4u);
		struct adsp_fastrpc_fd *slot = adsp_fastrpc_fd_get(fd);
		if (slot == NULL) {
			return EBADF;
		}
		if (op == APPS_STD_OP_FCLOSE) {
			adsp_fastrpc_free_fd_data(slot);
			memset(slot, 0, sizeof(*slot));
		} else if (op == APPS_STD_OP_REWIND) {
			slot->pos = 0;
			slot->eof = false;
		} else if (op == APPS_STD_OP_CLEARERR) {
			slot->eof = false;
		}
		return 0;
	}

	if (op == APPS_STD_OP_FREAD) {
		if (nin != 1u || nout != 2u || pra[0].len < 8u || pra[nin].len < 8u) {
			return EINVAL;
		}
		int32_t fd = 0;
		uint32_t want = 0;
		memcpy(&fd, pra[0].pv, 4u);
		memcpy(&want, pra[0].pv + 4u, 4u);
		if (want > pra[nin + 1u].len) {
			return EINVAL;
		}
		struct adsp_fastrpc_fd *slot = adsp_fastrpc_fd_get(fd);
		if (slot == NULL) {
			return EBADF;
		}
		uint64_t avail = slot->pos < slot->size ? (slot->size - slot->pos) : 0ULL;
		uint32_t take = (uint32_t)(avail < want ? avail : want);
		if (take > 0) {
			memcpy(pra[nin + 1u].pv, slot->data + slot->pos, take);
			slot->pos += take;
		}
		slot->eof = (take < want || slot->pos >= slot->size);
		int32_t b_eof = slot->eof ? 1 : 0;
		memcpy(pra[nin].pv, &take, 4u);
		memcpy(pra[nin].pv + 4u, &b_eof, 4u);
		return 0;
	}

	if (op == APPS_STD_OP_FLEN) {
		if (nin != 1u || nout != 1u || pra[0].len < 4u || pra[nin].len < 8u) {
			return EINVAL;
		}
		int32_t fd = 0;
		memcpy(&fd, pra[0].pv, 4u);
		struct adsp_fastrpc_fd *slot = adsp_fastrpc_fd_get(fd);
		if (slot == NULL) {
			return EBADF;
		}
		memcpy(pra[nin].pv, &slot->size, 8u);
		return 0;
	}

	if (op == APPS_STD_OP_FTELL || op == APPS_STD_OP_FEOF || op == APPS_STD_OP_FERROR) {
		if (nin != 1u || nout != 1u || pra[0].len < 4u || pra[nin].len < 4u) {
			return EINVAL;
		}
		int32_t fd = 0;
		memcpy(&fd, pra[0].pv, 4u);
		struct adsp_fastrpc_fd *slot = adsp_fastrpc_fd_get(fd);
		if (slot == NULL) {
			return EBADF;
		}
		int32_t outv = (op == APPS_STD_OP_FTELL) ? (int32_t)slot->pos :
		    (op == APPS_STD_OP_FEOF ? (slot->eof ? 1 : 0) : 0);
		memcpy(pra[nin].pv, &outv, 4u);
		return 0;
	}

	if (op == APPS_STD_OP_FSEEK) {
		if (nin != 1u || nout != 0u || pra[0].len < 12u) {
			return EINVAL;
		}
		int32_t fd = 0, off = 0, whence = 0;
		memcpy(&fd, pra[0].pv, 4u);
		memcpy(&off, pra[0].pv + 4u, 4u);
		memcpy(&whence, pra[0].pv + 8u, 4u);
		struct adsp_fastrpc_fd *slot = adsp_fastrpc_fd_get(fd);
		if (slot == NULL) {
			return EBADF;
		}
		int64_t base = (whence == 0) ? 0 : (whence == 1 ? (int64_t)slot->pos :
		    (whence == 2 ? (int64_t)slot->size : -1));
		if (base < 0 || base + off < 0 || (uint64_t)(base + off) > slot->size) {
			return EINVAL;
		}
		slot->pos = (uint64_t)(base + off);
		slot->eof = false;
		return 0;
	}

	if (op == APPS_STD_OP_FILE_EXISTS) {
		if (nin != 2u || nout != 1u || pra[0].len < 4u || pra[nin].len < 1u) {
			return EINVAL;
		}
		uint32_t plen = 0;
		memcpy(&plen, pra[0].pv, 4u);
		if (!adsp_sbuf_str_ok(&pra[1], plen)) {
			return EINVAL;
		}
		const char *path = (const char *)pra[1].pv;
		const uint8_t *data = NULL;
		uint64_t sz = 0;
		bool owned = false;
		uint8_t exists = adsp_fastrpc_lookup_sensor_file(path, &data, &sz, &owned) ? 1u : 0u;
		if (owned && data != NULL && sz > 0) {
			struct adsp_fastrpc_fd tmp = { .owned = true, .data = data, .size = sz };
			adsp_fastrpc_free_fd_data(&tmp);
		}
		if (exists == 0u) {
			adsp_fastrpc_log_missing("exists", path);
		}
		pra[nin].pv[0] = exists;
		return 0;
	}

	if (op == APPS_STD_OP_EXT && nin == 2u && nout == 1u && pra[0].len >= 8u && pra[nin].len >= 96u) {
		uint32_t mid = 0, plen = 0;
		memcpy(&mid, pra[0].pv, 4u);
		memcpy(&plen, pra[0].pv + 4u, 4u);
		if (mid == APPS_STD_EXT_STAT && adsp_sbuf_str_ok(&pra[1], plen)) {
			const char *path = (const char *)pra[1].pv;
			const uint8_t *data = NULL;
			uint64_t sz = 0;
			bool owned = false;
			if (!adsp_fastrpc_lookup_sensor_file(path, &data, &sz, &owned)) {
				adsp_fastrpc_log_missing("stat", path);
				return ENOENT;
			}
			struct apps_std_stat_wire st;
			memset(&st, 0, sizeof(st));
			st.mode = 0100444u;
			st.nlink = 1u;
			st.size = sz;
			memcpy(pra[nin].pv, &st, sizeof(st));
			if (owned && data != NULL && sz > 0) {
				struct adsp_fastrpc_fd tmp = { .owned = true, .data = data, .size = sz };
				adsp_fastrpc_free_fd_data(&tmp);
			}
			return 0;
		}
	}

	if (op == APPS_STD_OP_GETENV) {
		if (nin != 2u || nout != 1u || pra[0].len < 8u || pra[nin].len < 8u) {
			return EINVAL;
		}
		uint32_t nlen = 0, vlen = 0;
		memcpy(&nlen, pra[0].pv, 4u);
		memcpy(&vlen, pra[0].pv + 4u, 4u);
		if (!adsp_sbuf_str_ok(&pra[1], nlen)) {
			return EINVAL;
		}
		static const char sns_paths[] =
		    "/vendor/etc/sensors/config;/mnt/vendor/persist/sensors/registry;/fw/sensors";
		const char *var = (const char *)pra[1].pv;
		if (strcmp(var, "ADSP_LIBRARY_PATH") == 0 || strcmp(var, "ADSP_AVS_CFG_PATH") == 0) {
			uint32_t req = (uint32_t)sizeof(sns_paths);
			memcpy(pra[nin].pv, &req, 4u);
			if (vlen >= req && nout >= 2u && pra[nin + 1u].len >= req) {
				memcpy(pra[nin + 1u].pv, sns_paths, req);
			}
			return 0;
		}
		adsp_fastrpc_log_missing("getenv", var);
		return ENOENT;
	}

	if (op == APPS_STD_OP_OPENDIR) {
		if (nin != 2u || nout != 1u || pra[0].len < 4u || pra[nin].len < 8u) {
			return EINVAL;
		}
		uint32_t dlen = 0;
		memcpy(&dlen, pra[0].pv, 4u);
		if (!adsp_sbuf_str_ok(&pra[1], dlen)) {
			return EINVAL;
		}
		adsp_fastrpc_log_missing("opendir", (const char *)pra[1].pv);
		return ENOENT;
	}

	if (op == APPS_STD_OP_PRINT_STRING) {
		if (nin == 2u && pra[0].len >= 4u) {
			uint32_t slen = 0;
			memcpy(&slen, pra[0].pv, 4u);
			if (adsp_sbuf_str_ok(&pra[1], slen)) {
				say("fastrpc: dsp print: %.128s\n", (const char *)pra[1].pv);
				return 0;
			}
		}
		return EINVAL;
	}

	adsp_fastrpc_log_missing("op", "unsupported");
	return ENOSYS;
}

static int32_t
adsp_fastrpc_handle_remotectl(uint32_t sc, struct adsp_fastrpc_rev_arg *pra)
{
	uint32_t op = ADSP_SC_METHOD(sc);
	uint32_t nin = ADSP_SC_INBUFS(sc);
	uint32_t nout = ADSP_SC_OUTBUFS(sc);

	if (op == 0u && nin == 2u && nout == 2u && pra[0].len >= 8u && pra[nin].len >= 8u) {
		uint32_t slen = 0;
		memcpy(&slen, pra[0].pv, 4u);
		if (!adsp_sbuf_str_ok(&pra[1], slen)) {
			return EINVAL;
		}
		const char *uri = (const char *)pra[1].pv;
		size_t uri_len = strlen(uri);
		int32_t h = 0, err = 0;
		if (strnstr(uri, "apps_std", uri_len) != NULL) {
			h = (int32_t)ADSP_FASTRPC_REV_APPS_STD;
		} else if (strnstr(uri, "adspmsgd_apps", uri_len) != NULL) {
			h = (int32_t)ADSP_FASTRPC_REV_ADSPMSGD;
		} else {
			err = ENOENT;
			adsp_fastrpc_log_missing("remotectl_open", uri);
		}
		memcpy(pra[nin].pv, &h, 4u);
		memcpy(pra[nin].pv + 4u, &err, 4u);
		return 0;
	}
	if (op == 1u && nin == 1u && nout == 2u && pra[nin].len >= 4u) {
		int32_t err = 0;
		memcpy(pra[nin].pv, &err, 4u);
		return 0;
	}
	return ENOSYS;
}

static int32_t
adsp_fastrpc_dispatch_rev(uint32_t handle, uint32_t sc, const uint8_t *inbufs, uint32_t in_len,
    uint8_t *outbufs, uint32_t out_cap, uint32_t *out_len_used)
{
	struct adsp_fastrpc_rev_arg pra[ADSP_FASTRPC_MAX_ARGS];
	uint32_t out_req = 0;
	*out_len_used = 0;
	memset(pra, 0, sizeof(pra));

	if (!adsp_sbuf_unpack(inbufs, in_len, sc, pra, &out_req)) {
		return EINVAL;
	}
	if (out_req > 0 && !adsp_sbuf_bind_outputs(outbufs, out_cap, ADSP_SC_INBUFS(sc),
	    ADSP_SC_OUTBUFS(sc), pra, out_len_used)) {
		return ENOMEM;
	}
	if (handle == ADSP_FASTRPC_REV_REMOTECTL) {
		return adsp_fastrpc_handle_remotectl(sc, pra);
	}
	if (handle == ADSP_FASTRPC_REV_APPS_STD) {
		return adsp_fastrpc_handle_apps_std(sc, pra);
	}
	if (handle == ADSP_FASTRPC_REV_ADSPMSGD) {
		return 0;
	}
	adsp_fastrpc_log_missing("handle", "unknown");
	return ENOENT;
}

#endif /* _ANNIBALE_ADSP_FASTRPC_RPC_INC_ */

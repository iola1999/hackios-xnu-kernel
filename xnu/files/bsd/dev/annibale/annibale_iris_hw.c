/*
 * SM8750 Iris VPU v3.5 hardware bring-up:
 *   - Lazy firmware discovery (fw/vpu35_4v.mbn via annibale_image_file or VFS)
 *   - ELF32/ELF64 MBN parser & Qualcomm PAS 9 authentication into video_mem@0x9c300000
 *   - RPMh MMCX/MXC active TCS voting, GCC video branches, VIDEO_CC PLL0 & MVS0C/MVS0 GDSCs
 *   - apps-smmu@0x15000000 3-level 39-bit Stage-1 context bank for SID 0x1940 & SID 0x1947
 *   - VPU35 register presets, HFI queue table setup, and CTRL_INIT handshake
 */
#include "annibale_iris_private.h"

#define IRIS_MBN_META_MAX       0x4000u
#define IRIS_MBN_MAX_SEGS       32u

/* SMMU Stage-1 PTE flags (MAIR0 = 0xf404ff44: idx0=NC, idx1=WBWA, idx2=Device-nGnRE) */
#define IRIS_PTE_TABLE          3ULL
#define IRIS_PTE_ADDR_MASK      0x0000fffffffff000ULL
#define IRIS_PTE_NORMAL_NC      (3ULL | (0ULL << 2) | (3ULL << 8) | (1ULL << 10) | (1ULL << 11))
#define IRIS_PTE_NORMAL_WB      (3ULL | (1ULL << 2) | (3ULL << 8) | (1ULL << 10) | (1ULL << 11))
#define IRIS_PTE_DEVICE_XN      (3ULL | (2ULL << 2) | (1ULL << 10) | (1ULL << 11) | (3ULL << 53))

struct iris_mbn_seg {
	uint32_t file_off;
	uint32_t file_sz;
	uint64_t paddr;
	uint32_t mem_sz;
};

struct iris_mbn_image {
	uint32_t header_size;
	uint32_t hash_off;
	uint32_t hash_size;
	uint64_t min_paddr;
	uint32_t total_mem_size;
	uint32_t seg_count;
	struct iris_mbn_seg segs[IRIS_MBN_MAX_SEGS];
};

static inline uint32_t
mmio_r32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile const uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mmio_w32(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

void
iris_sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

void
iris_cache_clean_inval(const void *addr, size_t len)
{
	uintptr_t cur = (uintptr_t)addr & ~63UL;
	uintptr_t end = ((uintptr_t)addr + len + 63UL) & ~63UL;

	__asm__ volatile("dsb sy" ::: "memory");
	for (; cur < end; cur += 64UL) {
		__asm__ volatile("dc civac, %0" :: "r"(cur) : "memory");
	}
	__asm__ volatile("dsb sy\nisb" ::: "memory");
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static bool
iris_mbn_parse(const uint8_t *data, size_t size, struct iris_mbn_image *img)
{
	uint64_t phoff = 0, max_end = 0;
	uint16_t ehsize = 0, phentsize = 0, phnum = 0;
	bool is64 = false, hash_found = false;

	if (data == NULL || img == NULL || size < 64 || memcmp(data, "\177ELF", 4) != 0 || data[5] != 1) {
		return false;
	}
	memset(img, 0, sizeof(*img));
	img->min_paddr = ~0ULL;

	if (data[4] == 1) {
		is64 = false;
		phoff = le32(data + 28);
		ehsize = le16(data + 40);
		phentsize = le16(data + 42);
		phnum = le16(data + 44);
		if (ehsize != 52 || phentsize != 32) {
			return false;
		}
	} else if (data[4] == 2) {
		is64 = true;
		phoff = le64(data + 32);
		ehsize = le16(data + 52);
		phentsize = le16(data + 54);
		phnum = le16(data + 56);
		if (ehsize != 64 || phentsize != 56) {
			return false;
		}
	} else {
		return false;
	}

	if (phnum < 2 || phnum > 64 || phoff < ehsize || phoff + (uint64_t)phnum * phentsize > size) {
		return false;
	}
	const uint8_t *ph0 = data + phoff;
	img->header_size = is64 ? (uint32_t)le64(ph0 + 32) : le32(ph0 + 16);
	if (img->header_size < phoff + (uint32_t)phnum * phentsize || img->header_size > size) {
		return false;
	}

	for (uint16_t i = 1; i < phnum; i++) {
		const uint8_t *ph = data + phoff + (size_t)i * phentsize;
		uint32_t type = le32(ph);
		uint32_t flags = is64 ? le32(ph + 4) : le32(ph + 24);
		uint64_t off = is64 ? le64(ph + 8) : le32(ph + 4);
		uint64_t paddr = is64 ? le64(ph + 24) : le32(ph + 12);
		uint64_t filesz = is64 ? le64(ph + 32) : le32(ph + 16);
		uint64_t memsz = is64 ? le64(ph + 40) : le32(ph + 20);

		if ((flags & 0x07000000u) == 0x02000000u) {
			if (hash_found || filesz == 0 || off + filesz > size) {
				return false;
			}
			img->hash_off = (uint32_t)off;
			img->hash_size = (uint32_t)filesz;
			hash_found = true;
			continue;
		}
		if (type != 1u || memsz == 0) {
			continue;
		}
		if (img->seg_count >= IRIS_MBN_MAX_SEGS || filesz > memsz ||
		    off + filesz > size || memsz > IRIS_VIDEO_MEM_SIZE) {
			return false;
		}
		struct iris_mbn_seg *s = &img->segs[img->seg_count++];
		s->file_off = (uint32_t)off;
		s->file_sz = (uint32_t)filesz;
		s->paddr = paddr;
		s->mem_sz = (uint32_t)memsz;
		if (paddr < img->min_paddr) {
			img->min_paddr = paddr;
		}
		if (paddr + memsz > max_end) {
			max_end = paddr + memsz;
		}
	}

	if (!hash_found || img->seg_count == 0 || (img->min_paddr & 0xfffULL) != 0 ||
	    img->header_size + img->hash_size > IRIS_MBN_META_MAX || max_end <= img->min_paddr) {
		return false;
	}
	uint64_t span = ((max_end + 0xfffULL) & ~0xfffULL) - img->min_paddr;
	if (span == 0 || span > IRIS_VIDEO_MEM_SIZE) {
		return false;
	}
	img->total_mem_size = (uint32_t)span;
	return true;
}

static const uint8_t *
iris_vfs_read_fw(const char *path, uint64_t *size_out)
{
	vfs_context_t ctx = vfs_context_current();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	uint8_t *buf = NULL;
	int resid = 0;

	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
		return NULL;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (vnode_getattr(vp, &va, ctx) != 0 || va.va_data_size < 4096 || va.va_data_size > 8 * 1024 * 1024) {
		vnode_close(vp, FREAD, ctx);
		return NULL;
	}
	buf = kalloc_data((size_t)va.va_data_size, Z_WAITOK | Z_ZERO);
	if (buf == NULL) {
		vnode_close(vp, FREAD, ctx);
		return NULL;
	}
	if (vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)va.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) != 0 || resid != 0) {
		kfree_data(buf, (size_t)va.va_data_size);
		vnode_close(vp, FREAD, ctx);
		return NULL;
	}
	vnode_close(vp, FREAD, ctx);
	g_iris.fw_vfs_buf = buf;
	*size_out = va.va_data_size;
	return buf;
}

bool
iris_hw_probe_firmware(void)
{
	struct iris_mbn_image img;
	uint64_t sz = 0;
	int disk = 0;

	if (g_iris.fw_checked) {
		return g_iris.fw_present;
	}
	g_iris.fw_checked = true;

	const uint8_t *data = annibale_image_file("fw", "vpu35_4v.mbn", &sz, &disk);
	if (data == NULL) {
		data = iris_vfs_read_fw("/annibale/fw/vpu35_4v.mbn", &sz);
	}
	if (data == NULL) {
		data = iris_vfs_read_fw("/private/var/firmware/vpu35_4v.mbn", &sz);
	}
	if (data == NULL && annibale_ufs_fat16_fw_read("vpu35_4v.mbn", &sz, NULL, 0) &&
	    sz >= 4096 && sz <= 8 * 1024 * 1024) {
		uint8_t *buf = kalloc_data((size_t)sz, Z_WAITOK | Z_ZERO);
		if (buf != NULL) {
			if (annibale_ufs_fat16_fw_read("vpu35_4v.mbn", &sz, buf, sz)) {
				g_iris.fw_vfs_buf = buf;
				data = buf;
			} else {
				kfree_data(buf, (size_t)sz);
			}
		}
	}
	if (data == NULL || !iris_mbn_parse(data, (size_t)sz, &img)) {
		iris_say("vpu35_4v.mbn not present or invalid (sz=%llu)\n", (unsigned long long)sz);
		return false;
	}
	g_iris.fw_data = data;
	g_iris.fw_size = (uint32_t)sz;
	g_iris.fw_present = true;
	return true;
}

/* ---- SMMU Stage-1 3-level page table (39-bit IOVA, 4 KiB granule) ---------------------------- */

static vm_offset_t
iris_smmu_get_table(uint8_t level, uint32_t key, uint64_t *pa_out)
{
	for (uint32_t i = 0; i < g_iris.table_count; i++) {
		if (g_iris.tables[i].level == level && g_iris.tables[i].key == key) {
			*pa_out = g_iris.tables[i].pa;
			return g_iris.tables[i].kva;
		}
	}
	if (g_iris.table_count >= IRIS_SMMU_MAX_TABLES) {
		return 0;
	}
	vm_offset_t kva = 0;
	if (kmem_alloc(kernel_map, &kva, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		return 0;
	}
	uint64_t pa = kvtophys(kva);
	if (pa == 0 || (pa & 0xfffULL) != 0 || (pa & ~IRIS_PTE_ADDR_MASK) != 0) {
		kmem_free(kernel_map, kva, PAGE_SIZE);
		return 0;
	}
	iris_cache_clean_inval((const void *)kva, 4096u);
	struct iris_smmu_table *t = &g_iris.tables[g_iris.table_count++];
	t->kva = kva;
	t->pa = pa;
	t->key = key;
	t->level = level;
	*pa_out = pa;
	return kva;
}

static void
iris_smmu_tlb_sync(void)
{
	if (!g_iris.smmu_ready || g_iris.smmu == 0) {
		return;
	}
	uint32_t cb_off = (g_iris.smmu_pages + g_iris.smmu_cb) * g_iris.smmu_page;
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TLBIASID, g_iris.smmu_cb);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TLBSYNC, 0);
	for (uint32_t i = 0; i < 2000u; i++) {
		if ((mmio_r32(g_iris.smmu, cb_off + IRIS_CB_TLBSTATUS) & 1u) == 0) {
			return;
		}
		iris_sleep_us(2);
	}
}

static bool
iris_smmu_map_page(uint32_t iova, uint64_t pa, uint64_t attrs)
{
	uint64_t root_pa = 0, l2_pa = 0, l3_pa = 0;

	if ((pa & 0xfffULL) != 0 || (pa & ~IRIS_PTE_ADDR_MASK) != 0) {
		return false;
	}
	vm_offset_t l1 = iris_smmu_get_table(0, 0, &root_pa);
	if (l1 == 0) {
		return false;
	}
	uint32_t idx1 = (iova >> 30) & 0x1ffu;
	uint32_t idx2 = (iova >> 21) & 0x1ffu;
	uint32_t idx3 = (iova >> 12) & 0x1ffu;

	vm_offset_t l2 = iris_smmu_get_table(1, idx1, &l2_pa);
	vm_offset_t l3 = iris_smmu_get_table(2, (idx1 << 9) | idx2, &l3_pa);
	if (l2 == 0 || l3 == 0) {
		return false;
	}

	uint64_t *p1 = (uint64_t *)l1;
	uint64_t *p2 = (uint64_t *)l2;
	uint64_t *p3 = (uint64_t *)l3;
	if (p1[idx1] == 0) {
		p1[idx1] = (l2_pa & IRIS_PTE_ADDR_MASK) | IRIS_PTE_TABLE;
		iris_cache_clean_inval(&p1[idx1], sizeof(uint64_t));
	}
	if (p2[idx2] == 0) {
		p2[idx2] = (l3_pa & IRIS_PTE_ADDR_MASK) | IRIS_PTE_TABLE;
		iris_cache_clean_inval(&p2[idx2], sizeof(uint64_t));
	}
	p3[idx3] = (pa & IRIS_PTE_ADDR_MASK) | attrs;
	iris_cache_clean_inval(&p3[idx3], sizeof(uint64_t));
	return true;
}

static void
iris_smmu_unmap_range(uint32_t iova, uint32_t size)
{
	for (uint32_t off = 0; off < size; off += 4096u) {
		uint32_t va = iova + off;
		uint32_t key2 = (((va >> 30) & 0x1ffu) << 9) | ((va >> 21) & 0x1ffu);
		uint32_t idx3 = (va >> 12) & 0x1ffu;
		for (uint32_t i = 0; i < g_iris.table_count; i++) {
			if (g_iris.tables[i].level == 2 && g_iris.tables[i].key == key2) {
				uint64_t *p3 = (uint64_t *)g_iris.tables[i].kva;
				p3[idx3] = 0;
				iris_cache_clean_inval(&p3[idx3], sizeof(uint64_t));
				break;
			}
		}
	}
	iris_smmu_tlb_sync();
}

bool
iris_dma_alloc_map(struct iris_dma_buf *buf, uint32_t iova, uint32_t size, bool cached)
{
	uint32_t aligned = (size + (uint32_t)PAGE_MASK) & ~(uint32_t)PAGE_MASK;
	vm_offset_t kva = 0;

	memset(buf, 0, sizeof(*buf));
	if (aligned == 0 || kmem_alloc(kernel_map, &kva, aligned, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		return false;
	}
	iris_cache_clean_inval((const void *)kva, aligned);

	uint64_t attrs = cached ? IRIS_PTE_NORMAL_WB : IRIS_PTE_NORMAL_NC;
	for (uint32_t off = 0; off < aligned; off += 4096u) {
		uint64_t pa = kvtophys(kva + off);
		if (!iris_smmu_map_page(iova + off, pa, attrs)) {
			iris_smmu_unmap_range(iova, off);
			kmem_free(kernel_map, kva, aligned);
			return false;
		}
	}
	iris_smmu_tlb_sync();
	buf->kva = kva;
	buf->iova = iova;
	buf->size = aligned;
	return true;
}

void
iris_dma_free_unmap(struct iris_dma_buf *buf)
{
	if (buf == NULL || buf->kva == 0 || buf->size == 0) {
		return;
	}
	iris_smmu_unmap_range(buf->iova, buf->size);
	kmem_free(kernel_map, buf->kva, buf->size);
	memset(buf, 0, sizeof(*buf));
}

static bool
iris_smmu_setup(void)
{
	if (g_iris.smmu_ready) {
		return true;
	}
	if (g_iris.smmu == 0) {
		g_iris.smmu = ml_io_map(IRIS_APPS_SMMU_PHYS, IRIS_APPS_SMMU_SIZE);
		if (g_iris.smmu == 0) {
			return false;
		}
	}

	uint32_t idr0 = mmio_r32(g_iris.smmu, IRIS_SMMU_IDR0);
	uint32_t idr1 = mmio_r32(g_iris.smmu, IRIS_SMMU_IDR1);
	if (idr0 == ~0u || idr1 == ~0u) {
		return false;
	}
	uint32_t page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	uint32_t pages = 2u << ((idr1 >> 28) & 7u);
	uint32_t streams = idr0 & 0xffu;
	uint32_t banks = idr1 & 0xffu;
	if (banks < 4 || streams < 8 || (pages + 1u) * page > IRIS_APPS_SMMU_SIZE) {
		return false;
	}
	g_iris.smmu_page = page;
	g_iris.smmu_pages = pages;

	/* Locate existing or free SMR slots for SID 0x1940 and SID 0x1947. */
	int smr_non_pix = -1, smr_pix = -1;
	uint64_t used_cbs[2] = { 0, 0 };
	for (uint32_t i = 0; i < streams; i++) {
		uint32_t smr = mmio_r32(g_iris.smmu, IRIS_SMMU_SMR(i));
		uint32_t s2cr = mmio_r32(g_iris.smmu, IRIS_SMMU_S2CR(i));
		if ((smr & (1u << 31)) != 0) {
			uint32_t sid = smr & 0x7fffu;
			if (sid == IRIS_SID_NON_PIXEL) {
				smr_non_pix = (int)i;
			} else if (sid == IRIS_SID_PIXEL) {
				smr_pix = (int)i;
			} else {
				uint32_t cb_idx = s2cr & 0x7fu;
				used_cbs[cb_idx >> 6] |= (1ULL << (cb_idx & 63u));
			}
		}
	}
	for (uint32_t i = 0; i < streams && (smr_non_pix < 0 || smr_pix < 0); i++) {
		uint32_t smr = mmio_r32(g_iris.smmu, IRIS_SMMU_SMR(i));
		if ((smr & (1u << 31)) == 0) {
			if (smr_non_pix < 0) {
				smr_non_pix = (int)i;
			} else if (smr_pix < 0 && (int)i != smr_non_pix) {
				smr_pix = (int)i;
			}
		}
	}
	if (smr_non_pix < 0 || smr_pix < 0) {
		iris_say("no free apps-smmu SMR slots for 0x1940/0x1947\n");
		return false;
	}

	/*
	 * Pick an HLOS-usable Stage-1 context bank searching downward from banks - 2
	 * (banks - 1 is the shared bypass bank). Probe GR1_CBAR first as qcom_smmu_cfg_probe
	 * does so HYP-protected secure context bank pages are never touched.
	 */
	uint32_t gr1_off = page;
	uint32_t cbar_s1 = (1u << 16) | (0xfu << 12) | (3u << 8);
	int chosen_cb = -1;
	for (uint32_t idx = banks - 1u; idx > 1u; idx--) {
		uint32_t cb = idx - 1u;
		if (cb >= 128u || (pages + cb + 1u) * page > IRIS_APPS_SMMU_SIZE ||
		    (used_cbs[cb >> 6] & (1ULL << (cb & 63u))) != 0) {
			continue;
		}
		uint32_t old_cbar = mmio_r32(g_iris.smmu, gr1_off + IRIS_SMMU_GR1_CBAR(cb));
		if ((old_cbar & 0x30000u) == 0x30000u) {
			continue;
		}
		mmio_w32(g_iris.smmu, gr1_off + IRIS_SMMU_GR1_CBAR(cb), cbar_s1);
		if ((mmio_r32(g_iris.smmu, gr1_off + IRIS_SMMU_GR1_CBAR(cb)) & 0x30000u) != 0x10000u) {
			continue;
		}
		uint32_t cb_off = (pages + cb) * page;
		if (mmio_r32(g_iris.smmu, cb_off + IRIS_CB_SCTLR) == 0) {
			chosen_cb = (int)cb;
			break;
		}
	}
	if (chosen_cb < 0) {
		iris_say("no free apps-smmu context bank for Iris\n");
		return false;
	}

	uint64_t root_pa = 0;
	if (iris_smmu_get_table(0, 0, &root_pa) == 0) {
		return false;
	}
	if (!iris_smmu_map_page(IRIS_IOVA_DEV_QTIMER, IRIS_PHYS_DEV_QTIMER, IRIS_PTE_DEVICE_XN) ||
	    !iris_smmu_map_page(IRIS_IOVA_DEV_QDSS, IRIS_PHYS_DEV_QDSS, IRIS_PTE_DEVICE_XN)) {
		return false;
	}

	uint32_t cb = (uint32_t)chosen_cb;
	uint32_t cb_off = (pages + cb) * page;
	uint32_t s2cr_fault = (2u << 16) | (3u << 8);
	uint32_t smr0_val = (1u << 31) | IRIS_SID_NON_PIXEL;
	uint32_t smr1_val = (1u << 31) | IRIS_SID_PIXEL;

	mmio_w32(g_iris.smmu, IRIS_SMMU_S2CR((uint32_t)smr_non_pix), s2cr_fault);
	mmio_w32(g_iris.smmu, IRIS_SMMU_SMR((uint32_t)smr_non_pix), smr0_val);
	mmio_w32(g_iris.smmu, IRIS_SMMU_S2CR((uint32_t)smr_pix), s2cr_fault);
	mmio_w32(g_iris.smmu, IRIS_SMMU_SMR((uint32_t)smr_pix), smr1_val);
	if (mmio_r32(g_iris.smmu, IRIS_SMMU_SMR((uint32_t)smr_non_pix)) != smr0_val ||
	    mmio_r32(g_iris.smmu, IRIS_SMMU_SMR((uint32_t)smr_pix)) != smr1_val) {
		iris_say("apps-smmu rejected SID 0x1940/0x1947 SMR programming\n");
		return false;
	}

	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_SCTLR, 0u);
	mmio_w32(g_iris.smmu, gr1_off + IRIS_SMMU_GR1_CBA2R(cb), 1u);
	mmio_w32(g_iris.smmu, gr1_off + IRIS_SMMU_GR1_CBAR(cb), cbar_s1);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TCR2, 0x00038011u);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TCR, 0x00803519u);
	uint64_t ttbr0 = (root_pa & IRIS_PTE_ADDR_MASK) | ((uint64_t)cb << 48);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TTBR0, (uint32_t)ttbr0);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TTBR0 + 4u, (uint32_t)(ttbr0 >> 32));
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TTBR1, 0u);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TTBR1 + 4u, 0u);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_MAIR0, 0xf404ff44u);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_MAIR1, 0u);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_FSR, 0xc00001feu);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TLBIASID, cb);
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_TLBSYNC, 0u);
	for (uint32_t i = 0; i < 2000u; i++) {
		if ((mmio_r32(g_iris.smmu, cb_off + IRIS_CB_TLBSTATUS) & 1u) == 0) {
			break;
		}
		iris_sleep_us(2);
	}
	mmio_w32(g_iris.smmu, cb_off + IRIS_CB_SCTLR, 0x27u);

	uint32_t s2cr_val = (3u << 8) | cb;
	mmio_w32(g_iris.smmu, IRIS_SMMU_S2CR((uint32_t)smr_non_pix), s2cr_val);
	mmio_w32(g_iris.smmu, IRIS_SMMU_S2CR((uint32_t)smr_pix), s2cr_val);

	g_iris.smmu_cb = cb;
	g_iris.smmu_smr_non_pix = (uint32_t)smr_non_pix;
	g_iris.smmu_smr_pix = (uint32_t)smr_pix;
	g_iris.smmu_ready = true;
	iris_smmu_tlb_sync();
	iris_say("apps-smmu CB %u armed for SID 0x1940 (SMR %d) & 0x1947 (SMR %d)\n",
	    cb, smr_non_pix, smr_pix);
	return true;
}

/* ---- Power, Clocks, PAS 9 Firmware Load, & Boot Handshake ------------------------------------ */

static bool
iris_enable_cbcr(vm_offset_t base, uint32_t off, bool check_off)
{
	mmio_w32(base, off, mmio_r32(base, off) | 1u);
	if (!check_off) {
		iris_sleep_us(20);
		return true;
	}
	for (uint32_t i = 0; i < 2000u; i++) {
		uint32_t v = mmio_r32(base, off);
		if ((v & (1u << 31)) == 0 || ((v >> 28) & 7u) == 2u) {
			return true;
		}
		iris_sleep_us(5);
	}
	iris_say("CBCR +%#x enable timeout (%#x)\n", off, mmio_r32(base, off));
	return false;
}

static void
iris_rcgr_enable_xo(vm_offset_t base, uint32_t cmd_off, uint32_t cfg_off)
{
	mmio_w32(base, cfg_off, 0x01u); /* src=0 (bi_tcxo 19.2 MHz), div=1 */
	mmio_w32(base, cmd_off, mmio_r32(base, cmd_off) | 0x03u); /* UPDATE | ROOT_EN */
	for (uint32_t i = 0; i < 500u; i++) {
		if ((mmio_r32(base, cmd_off) & 1u) == 0) {
			break;
		}
		iris_sleep_us(5);
	}
}

static bool
iris_enable_gdsc(vm_offset_t base, uint32_t off, const char *name)
{
	uint32_t v = mmio_r32(base, off);
	if ((v & (1u << 31)) != 0 && (v & 1u) == 0) {
		return true;
	}
	mmio_w32(base, off, v & ~3u);
	for (uint32_t i = 0; i < 5000u; i++) {
		if ((mmio_r32(base, off) & (1u << 31)) != 0) {
			return true;
		}
		iris_sleep_us(10);
	}
	iris_say("GDSC %s (+%#x) power-on timeout (%#x)\n", name, off, mmio_r32(base, off));
	return false;
}

static bool
iris_power_and_clocks(void)
{
	(void)annibale_rpmh_arc_vote("mmcx.lvl", ANNIBALE_RPMPD_LEVEL_NOM);
	(void)annibale_rpmh_arc_vote("mxc.lvl", ANNIBALE_RPMPD_LEVEL_NOM);

	if (g_iris.gcc == 0) {
		g_iris.gcc = ml_io_map(IRIS_GCC_PHYS, IRIS_GCC_SIZE);
	}
	if (g_iris.vidcc == 0) {
		g_iris.vidcc = ml_io_map(IRIS_VIDCC_PHYS, IRIS_VIDCC_SIZE);
	}
	if (g_iris.mmio == 0) {
		g_iris.mmio = ml_io_map(IRIS_MMIO_PHYS, IRIS_MMIO_SIZE);
	}
	if (g_iris.gcc == 0 || g_iris.vidcc == 0 || g_iris.mmio == 0) {
		return false;
	}

	/* Enable HLOS-owned GCC video AHB/XO branches before touching VIDEO_CC (0x0aaf0000) */
	if (!iris_enable_cbcr(g_iris.gcc, GCC_VIDEO_AHB_CBCR, true) ||
	    !iris_enable_cbcr(g_iris.gcc, GCC_VIDEO_XO_CBCR, true)) {
		return false;
	}

	/* Mirror video_cc_sun_probe AO & CFG_GDSCR presets */
	mmio_w32(g_iris.vidcc, VIDCC_MVS0C_CFG_GDSCR,
	    mmio_r32(g_iris.vidcc, VIDCC_MVS0C_CFG_GDSCR) | 0x3e00000u);
	mmio_w32(g_iris.vidcc, VIDCC_MVS0_CFG_GDSCR,
	    mmio_r32(g_iris.vidcc, VIDCC_MVS0_CFG_GDSCR) | 0x3e00000u);
	mmio_w32(g_iris.vidcc, VIDCC_MISC_AO_EN, mmio_r32(g_iris.vidcc, VIDCC_MISC_AO_EN) | 1u);
	iris_rcgr_enable_xo(g_iris.vidcc, VIDCC_AHB_CMD_RCGR, VIDCC_AHB_CFG_RCGR);
	(void)iris_enable_cbcr(g_iris.vidcc, VIDCC_AHB_CBCR, true);
	iris_rcgr_enable_xo(g_iris.vidcc, VIDCC_XO_CMD_RCGR, VIDCC_XO_CFG_RCGR);
	(void)iris_enable_cbcr(g_iris.vidcc, VIDCC_XO_CBCR, true);
	iris_rcgr_enable_xo(g_iris.vidcc, VIDCC_SLEEP_CMD_RCGR, VIDCC_SLEEP_CFG_RCGR);
	(void)iris_enable_cbcr(g_iris.vidcc, VIDCC_SLEEP_CBCR, true);

	if (!iris_enable_gdsc(g_iris.vidcc, VIDCC_MVS0C_GDSCR, "mvs0c")) {
		return false;
	}

	/*
	 * Configure & enable Lucid Ole video_cc_pll0 per clk_lucid_evo_pll_configure()
	 * and _alpha_pll_lucid_evo_enable() (video_cc_pll0_config: l=0x25, cal_l=0x44, alpha=0x8000).
	 */
	mmio_w32(g_iris.vidcc, VIDCC_PLL0_USER_CTL,
	    mmio_r32(g_iris.vidcc, VIDCC_PLL0_USER_CTL) | 7u);
	uint32_t pll_mode = mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE);
	uint32_t pll_opmode = mmio_r32(g_iris.vidcc, VIDCC_PLL0_OPMODE);
	if ((pll_mode & 1u) == 0 || (pll_opmode & 1u) == 0) {
		if ((mmio_r32(g_iris.vidcc, VIDCC_PLL0_L_VAL) & 0xffffu) == 0) {
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_MODE, pll_mode & ~4u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_L_VAL, 0x44440025u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_ALPHA_VAL, 0x8000u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_CONFIG_CTL, 0x19660387u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_CONFIG_CTL_U, 0x098060a0u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_CONFIG_CTL_U1, 0xb416cb20u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_USER_CTL, 7u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_USER_CTL_U, 2u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_TEST_CTL, 0u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_TEST_CTL_U, 0u);
			mmio_w32(g_iris.vidcc, VIDCC_PLL0_MODE,
			    mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE) & ~1u);
		}
		mmio_w32(g_iris.vidcc, VIDCC_PLL0_MODE,
		    mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE) | 4u);
		mmio_w32(g_iris.vidcc, VIDCC_PLL0_OPMODE, 1u);
		for (uint32_t i = 0; i < 2000u; i++) {
			if ((mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE) & (1u << 31)) != 0) {
				break;
			}
			iris_sleep_us(5);
		}
		mmio_w32(g_iris.vidcc, VIDCC_PLL0_MODE,
		    mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE) | 1u);
	}

	if ((mmio_r32(g_iris.vidcc, VIDCC_PLL0_MODE) & (1u << 31)) != 0) {
		mmio_w32(g_iris.vidcc, VIDCC_MVS0_CFG_RCGR, 0x102u);
		mmio_w32(g_iris.vidcc, VIDCC_MVS0_CMD_RCGR, mmio_r32(g_iris.vidcc, VIDCC_MVS0_CMD_RCGR) | 1u);
		for (uint32_t i = 0; i < 1000u; i++) {
			if ((mmio_r32(g_iris.vidcc, VIDCC_MVS0_CMD_RCGR) & 1u) == 0) {
				break;
			}
			iris_sleep_us(2);
		}
	}
	mmio_w32(g_iris.vidcc, VIDCC_MVS0_DIV_CDIVR, 1u);
	mmio_w32(g_iris.vidcc, VIDCC_MVS0C_DIV2_CDIVR, 1u);

	if (!iris_enable_cbcr(g_iris.vidcc, VIDCC_MVS0C_FREERUN_CBCR, true) ||
	    !iris_enable_cbcr(g_iris.vidcc, VIDCC_MVS0C_CBCR, true)) {
		return false;
	}
	(void)iris_enable_cbcr(g_iris.vidcc, VIDCC_MVS0C_SHIFT_CBCR, false);

	if (!iris_enable_gdsc(g_iris.vidcc, VIDCC_MVS0_GDSCR, "mvs0")) {
		return false;
	}
	/* video_cc_mvs0_freerun_clk enables via 0x808c bit 0 and polls halt status in 0x8090. */
	mmio_w32(g_iris.vidcc, VIDCC_MVS0_FREERUN_EN,
	    mmio_r32(g_iris.vidcc, VIDCC_MVS0_FREERUN_EN) | 1u);
	bool mvs0_freerun_ok = false;
	for (uint32_t i = 0; i < 2000u; i++) {
		uint32_t v = mmio_r32(g_iris.vidcc, VIDCC_MVS0_FREERUN_CBCR);
		if ((v & (1u << 31)) == 0 || ((v >> 28) & 7u) == 2u) {
			mvs0_freerun_ok = true;
			break;
		}
		iris_sleep_us(5);
	}
	if (!mvs0_freerun_ok) {
		iris_say("CBCR +%#x enable timeout (%#x)\n",
		    VIDCC_MVS0_FREERUN_CBCR, mmio_r32(g_iris.vidcc, VIDCC_MVS0_FREERUN_CBCR));
		return false;
	}
	if (!iris_enable_cbcr(g_iris.vidcc, VIDCC_MVS0_CBCR, true)) {
		return false;
	}
	(void)iris_enable_cbcr(g_iris.vidcc, VIDCC_MVS0_SHIFT_CBCR, false);

	/* Enable GCC video AXI0/1 branches after MVS0C and MVS0 GDSCs are awake. */
	if (!iris_enable_cbcr(g_iris.gcc, GCC_VIDEO_AXI0_CBCR, true) ||
	    !iris_enable_cbcr(g_iris.gcc, GCC_VIDEO_AXI1_CBCR, true)) {
		return false;
	}
	return true;
}

static bool
iris_pas_call(uint64_t cmd, uint64_t arginfo, uint64_t a1, uint64_t a2)
{
	uint64_t ans = ~0ULL;
	int32_t st = adreno8_scm_call(cmd, arginfo, IRIS_PAS_ID, a1, a2, 0, &ans);
	if (st != 0 || ans != 0) {
		iris_say("PAS 9 cmd %#llx failed: st=%d ans=%#llx\n",
		    (unsigned long long)cmd, st, (unsigned long long)ans);
		return false;
	}
	return true;
}

static bool
iris_pas_authenticate(void)
{
	struct iris_mbn_image img;

	if (g_iris.pas_authed) {
		return true;
	}
	if (!iris_hw_probe_firmware() || !iris_mbn_parse(g_iris.fw_data, g_iris.fw_size, &img)) {
		return false;
	}

	uint32_t alloc_sz = (uint32_t)(PAGE_SIZE >= IRIS_MBN_META_MAX ? PAGE_SIZE : 0x10000u);
	if (g_iris.pas_meta == 0 && kmem_alloc(kernel_map, &g_iris.pas_meta, alloc_sz,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		return false;
	}

	vm_offset_t meta_va = 0;
	uint64_t meta_pa = 0;
	for (uint32_t off = 0; off + IRIS_MBN_META_MAX <= alloc_sz; off += 4096u) {
		uint64_t p0 = kvtophys(g_iris.pas_meta + off);
		bool contig = (p0 != 0 && (p0 & 0xfffULL) == 0);
		for (uint32_t p = 4096u; contig && p < IRIS_MBN_META_MAX; p += 4096u) {
			if (kvtophys(g_iris.pas_meta + off + p) != p0 + p) {
				contig = false;
			}
		}
		if (contig) {
			meta_va = g_iris.pas_meta + off;
			meta_pa = p0;
			break;
		}
	}
	if (meta_pa == 0) {
		return false;
	}

	memset((void *)meta_va, 0, IRIS_MBN_META_MAX);
	memcpy((void *)meta_va, g_iris.fw_data, img.header_size);
	memcpy((void *)(meta_va + img.header_size), g_iris.fw_data + img.hash_off, img.hash_size);
	iris_cache_clean_inval((const void *)meta_va, IRIS_MBN_META_MAX);

	uint64_t ans = ~0ULL;
	int32_t st = adreno8_scm_call(0x42000201ULL, 0x82ULL, IRIS_PAS_ID, meta_pa, 0, 0, &ans);
	if (st != 0 || ans != 0) {
		st = adreno8_scm_call(0x42000201ULL, 0x22ULL, IRIS_PAS_ID, meta_pa, 0, 0, &ans);
	}
	if (st != 0 || ans != 0) {
		iris_say("PAS 9 INIT_IMAGE failed: st=%d ans=%#llx\n", st, (unsigned long long)ans);
		return false;
	}
	if (!iris_pas_call(0x42000202ULL, 3ULL, IRIS_VIDEO_MEM_PHYS, img.total_mem_size)) {
		return false;
	}

	volatile uint8_t *carveout = (volatile uint8_t *)ml_io_map_unmappable(
	    (vm_offset_t)IRIS_VIDEO_MEM_PHYS, img.total_mem_size, VM_WIMG_WCOMB);
	if (carveout == NULL) {
		iris_say("PAS 9 carveout map failed (%#x bytes)\n", img.total_mem_size);
		return false;
	}
	for (uint32_t i = 0; i < img.seg_count; i++) {
		const struct iris_mbn_seg *s = &img.segs[i];
		uint32_t at = (uint32_t)(s->paddr - img.min_paddr);
		const uint8_t *src = g_iris.fw_data + s->file_off;
		for (uint32_t j = 0; j < s->file_sz; j++) {
			carveout[at + j] = src[j];
		}
		uint32_t cur = at + s->file_sz;
		uint32_t end = at + s->mem_sz;
		while (cur < end && ((uintptr_t)(carveout + cur) & 7UL) != 0) {
			carveout[cur++] = 0;
		}
		while (cur + 8u <= end) {
			*(volatile uint64_t *)(carveout + cur) = 0ULL;
			cur += 8u;
		}
		while (cur < end) {
			carveout[cur++] = 0;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	ml_io_unmap((vm_offset_t)carveout, img.total_mem_size);

	if (!iris_pas_call(0x42000205ULL, 1ULL, 0, 0)) {
		return false;
	}

	/* Optional CP non-pixel range notification (qcom_scm_mem_protect_video_var). */
	uint64_t mp_ans = 0;
	(void)adreno8_scm_call(0x42000c08ULL, 4ULL, 0, 0x25800000ULL, 0x01000000ULL, 0x24800000ULL, &mp_ans);

	g_iris.pas_authed = true;
	iris_say("PAS 9 authentication succeeded (%u segments, %#x bytes)\n",
	    img.seg_count, img.total_mem_size);
	return true;
}

static bool
iris_boot_controller(void)
{
	static const struct { uint32_t off, val; } presets[] = {
		{ VPU_WRAPPER_TZ_CPU_STATUS, 0x00000000u },
		{ 0x13030u, 0x33332211u },
		{ 0x13034u, 0x44444444u },
		{ 0x13038u, 0x00001011u },
		{ 0x13040u, 0xffaa5500u },
		{ 0x13048u, 0x000000ffu },
		{ 0x13430u, 0x33332211u },
		{ 0x13434u, 0x44444444u },
		{ 0x13438u, 0x00001011u },
		{ 0x13440u, 0xffaa5500u },
		{ 0x13448u, 0x000000ffu },
		{ 0xa013cu, 0x00000099u },
	};

	if (g_iris.uc_region.kva == 0 &&
	    !iris_dma_alloc_map(&g_iris.uc_region, IRIS_IOVA_UC_BASE, IRIS_IOVA_UC_SIZE, false)) {
		return false;
	}
	if (g_iris.sfr_region.kva == 0 &&
	    !iris_dma_alloc_map(&g_iris.sfr_region, IRIS_IOVA_SFR_BASE, IRIS_IOVA_SFR_SIZE, false)) {
		return false;
	}

	memset((void *)g_iris.uc_region.kva, 0, g_iris.uc_region.size);
	memset((void *)g_iris.sfr_region.kva, 0, g_iris.sfr_region.size);

	struct hfi_queue_table_header *tbl = (struct hfi_queue_table_header *)g_iris.uc_region.kva;
	tbl->version = 0;
	tbl->size = 0x1c8u;
	tbl->qhdr0_offset = 0x120u;
	tbl->qhdr_size = sizeof(struct hfi_queue_header);
	tbl->num_q = 3u;
	tbl->num_active_q = 3u;

	struct hfi_queue_header *qhdr = (struct hfi_queue_header *)(g_iris.uc_region.kva + 0x120u);
	const uint32_t q_starts[3] = {
		IRIS_IOVA_UC_BASE + IRIS_HFI_CMDQ_OFF,
		IRIS_IOVA_UC_BASE + IRIS_HFI_MSGQ_OFF,
		IRIS_IOVA_UC_BASE + IRIS_HFI_DBGQ_OFF,
	};
	for (uint32_t i = 0; i < 3u; i++) {
		qhdr[i].status = 1u;
		qhdr[i].start_addr = q_starts[i];
		qhdr[i].type = 0x01010000u | i;
		qhdr[i].q_size = IRIS_HFI_Q_DWORDS;
		qhdr[i].pkt_size = 0;
		qhdr[i].rx_wm = 1u;
		qhdr[i].tx_wm = 1u;
		qhdr[i].rx_req = 1u;
		qhdr[i].tx_req = 0u;
	}
	*(uint32_t *)g_iris.sfr_region.kva = IRIS_IOVA_SFR_SIZE;
	iris_cache_clean_inval((const void *)g_iris.uc_region.kva, g_iris.uc_region.size);
	iris_cache_clean_inval((const void *)g_iris.sfr_region.kva, g_iris.sfr_region.size);

	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		mmio_w32(g_iris.mmio, presets[i].off, presets[i].val);
	}
	mmio_w32(g_iris.mmio, VPU_WRAPPER_INTR_MASK,
	    mmio_r32(g_iris.mmio, VPU_WRAPPER_INTR_MASK) & ~0xcu);

	mmio_w32(g_iris.mmio, VPU_CPU_CS_UC_REGION_ADDR, IRIS_IOVA_UC_BASE);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_UC_REGION_SIZE, IRIS_IOVA_UC_SIZE);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_SCIACMDARG2, IRIS_IOVA_UC_BASE);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_SCIACMDARG1, 1u);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_VCICMDARG0, IRIS_IOVA_DEV_QTIMER);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_VCICMDARG1, 0x2000u);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_SFR_ADDR, IRIS_IOVA_SFR_BASE | 1u);
	mmio_w32(g_iris.mmio, VPU_WRAPPER_CPU_STATUS, 1u);
	mmio_w32(g_iris.mmio, VPU_CPU_CS_SCIACMD, 1u);

	for (uint32_t i = 0; i < 2000u; i++) {
		uint32_t st = mmio_r32(g_iris.mmio, VPU_CPU_CS_SCIACMDARG0);
		if ((st & 0xeu) != 0) {
			iris_say("CTRL_STATUS error %#x\n", st);
			return false;
		}
		if ((st & 1u) != 0) {
			mmio_w32(g_iris.mmio, VPU_CPU_CS_H2XSOFTINTEN, 1u);
			mmio_w32(g_iris.mmio, VPU_CPU_CS_X2RPMH, 0u);
			return iris_hfi_sys_init();
		}
		iris_sleep_us(250);
	}
	iris_say("CTRL_STATUS ready timeout (%#x)\n", mmio_r32(g_iris.mmio, VPU_CPU_CS_SCIACMDARG0));
	return false;
}

bool
iris_hw_ensure_ready(void)
{
	uint32_t hw_enable = 1u;

	if (g_iris.hw_ready) {
		return true;
	}
	if (g_iris.hw_attempted) {
		return false;
	}
	if (PE_parse_boot_argn("iris_hw", &hw_enable, sizeof(hw_enable)) && hw_enable == 0) {
		g_iris.hw_attempted = true;
		iris_say("hardware decode disabled via boot-arg iris_hw=0\n");
		return false;
	}
	if (!iris_hw_probe_firmware()) {
		g_iris.hw_attempted = true;
		return false;
	}

	g_iris.hw_attempted = true;
	iris_say("power/clocks begin\n");
	if (!iris_power_and_clocks()) {
		iris_say("power/clocks failed\n");
		return false;
	}
	iris_say("SMMU begin\n");
	if (!iris_smmu_setup()) {
		iris_say("SMMU failed\n");
		return false;
	}
	iris_say("PAS begin\n");
	if (!iris_pas_authenticate()) {
		iris_say("PAS failed\n");
		return false;
	}
	iris_say("HFI begin\n");
	if (!iris_boot_controller()) {
		iris_say("HFI failed\n");
		return false;
	}

	g_iris.hw_ready = true;
	iris_say("SM8750 Iris VPU v3.5 online (HFI Gen2 ready)\n");
	return true;
}

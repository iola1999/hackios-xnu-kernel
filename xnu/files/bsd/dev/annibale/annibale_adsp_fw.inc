#ifndef _ANNIBALE_ADSP_FW_INC_
#define _ANNIBALE_ADSP_FW_INC_

/* ---- TrustZone PAS SMC calls & Firmware Loader ----------------------------------------------- */

static int scm_pas_call(uint64_t fn, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *res1)
{
	uint64_t x0 = fn, x1 = arginfo, x2 = a0, x3 = a1, x4 = a2, x6 = 0, r1 = 0;
	for (unsigned tries = 0; tries < 2000; tries++) {
		register uint64_t rx0 __asm__("x0") = x0, rx1 __asm__("x1") = x1;
		register uint64_t rx2 __asm__("x2") = x2, rx3 __asm__("x3") = x3, rx4 __asm__("x4") = x4;
		register uint64_t rx6 __asm__("x6") = x6;
		__asm__ volatile("smc #0"
		    : "+r"(rx0), "+r"(rx1), "+r"(rx2), "+r"(rx3), "+r"(rx4), "+r"(rx6)
		    : : "x5", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		if (rx0 == 1) { x0 = rx0; x1 = rx1; x2 = rx2; x3 = rx3; x4 = rx4; x6 = rx6; sleep_ms(1); continue; }
		if (rx0 == 2 || rx0 == 6) { x0 = fn; x1 = arginfo; x2 = a0; x3 = a1; x4 = a2; x6 = 0; sleep_ms(1); continue; }
		r1 = rx1;
		x0 = rx0;
		break;
	}
	if (res1 != NULL) *res1 = r1;
	return (int)(int64_t)x0;
}

static const uint8_t *fw_read_file(const char *name, uint64_t *size_out, bool *from_root_out)
{
	*from_root_out = false;
	const uint8_t *img = annibale_image_file("fw", name, size_out, NULL);
	if (img != NULL) return img;
	if (rootvnode != NULLVP) {
		static const char *const prefixes[] = { "/fw", "/annibale/fw" };
		vfs_context_t ctx = vfs_context_kernel();
		for (unsigned p = 0; p < 2; p++) {
			char path[80];
			vnode_t vp = NULLVP;
			struct vnode_attr va;
			snprintf(path, sizeof(path), "%s/%s", prefixes[p], name);
			if (vnode_lookup(path, 0, &vp, ctx) != 0) continue;
			VATTR_INIT(&va);
			VATTR_WANTED(&va, va_data_size);
			if (vnode_getattr(vp, &va, ctx) != 0 || va.va_data_size == 0 || va.va_data_size > ADSP_MEMORY_SIZE) {
				vnode_put(vp);
				continue;
			}
			uint8_t *buf = kalloc_data(va.va_data_size, Z_WAITOK);
			if (buf == NULL) { vnode_put(vp); return NULL; }
			int resid = 0;
			int err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)va.va_data_size, 0, UIO_SYSSPACE, 0,
			    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
			vnode_put(vp);
			if (err != 0 || resid != 0) {
				kfree_data(buf, va.va_data_size);
				continue;
			}
			*size_out = va.va_data_size;
			*from_root_out = true;
			return buf;
		}
	}
	uint64_t fat_sz = 0;
	if (annibale_ufs_fat16_fw_read(name, &fat_sz, NULL, 0) && fat_sz > 0 && fat_sz <= PAGE_SIZE * 4) {
		uint8_t *buf = kalloc_data(fat_sz, Z_WAITOK);
		if (buf != NULL) {
			if (annibale_ufs_fat16_fw_read(name, &fat_sz, buf, fat_sz)) {
				*size_out = fat_sz;
				*from_root_out = true;
				return buf;
			}
			kfree_data(buf, fat_sz);
		}
	}
	return NULL;
}

static void fw_release(struct fw_image *fw)
{
	if (fw->header != NULL && fw->from_root) {
		void *p = __DECONST(void *, fw->header);
		kfree_data(p, fw->header_size);
	}
	for (unsigned i = 0; i < fw->segments; i++) {
		if (fw->segment[i].file != NULL && fw->segment[i].from_root) {
			void *p = __DECONST(void *, fw->segment[i].file);
			kfree_data(p, fw->segment[i].file_size);
		}
	}
	bzero(fw, sizeof(*fw));
}

static bool fw_load_mdt(const char *stem, uint32_t region_base, uint32_t region_size, struct fw_image *fw)
{
	char mdt_name[32];
	uint64_t sz = 0;
	bool from_root = false;

	bzero(fw, sizeof(*fw));
	snprintf(mdt_name, sizeof(mdt_name), "%s.mdt", stem);
	fw->header = fw_read_file(mdt_name, &sz, &from_root);
	if (fw->header == NULL) return false;
	fw->header_size = sz;
	fw->from_root = from_root;

	uint32_t count = sz >= 52 + 2 * 32 ? (uint32_t)(fw->header[44] | (fw->header[45] << 8)) : 0;
	if (memcmp(fw->header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS ||
	    52 + 32 * count > sz || sz > PAGE_SIZE * 4) {
		fw_release(fw);
		return false;
	}

	uint32_t min_paddr = ~0u;
	bool reloc = false;
	for (uint32_t i = 0; i < count; i++) {
		struct elf_segment seg;
		memcpy(&seg, fw->header + 52 + 32 * i, sizeof(seg));
		if (seg.type != 1 || MDT_HASH(seg.flags) || seg.memory_size == 0) continue;
		if (MDT_RELOCATABLE(seg.flags)) reloc = true;
		if (seg.paddr < min_paddr) min_paddr = seg.paddr;
	}
	uint32_t base_paddr = reloc ? min_paddr : region_base, highest = 0;
	for (uint32_t i = 0; i < count; i++) {
		struct elf_segment seg;
		memcpy(&seg, fw->header + 52 + 32 * i, sizeof(seg));
		if (seg.type != 1 || MDT_HASH(seg.flags) || seg.memory_size == 0) continue;
		if (seg.paddr < base_paddr || seg.file_size > seg.memory_size ||
		    (uint64_t)(seg.paddr - base_paddr) + seg.memory_size > region_size) {
			fw_release(fw);
			return false;
		}
		uint32_t at = seg.paddr - base_paddr;
		fw->segment[fw->segments].at = at;
		fw->segment[fw->segments].file_size = seg.file_size;
		fw->segment[fw->segments].memory_size = seg.memory_size;
		fw->segment[fw->segments].seg_idx = i;
		if (seg.file_size != 0) {
			char bname[32];
			uint64_t bsz = 0;
			bool broot = false;
			snprintf(bname, sizeof(bname), "%s.b%02u", stem, i);
			fw->segment[fw->segments].file = fw_read_file(bname, &bsz, &broot);
			fw->segment[fw->segments].from_root = broot;
			if (fw->segment[fw->segments].file == NULL) {
				if (!annibale_ufs_fat16_fw_read(bname, &bsz, NULL, 0) || bsz != seg.file_size) {
					fw_release(fw);
					return false;
				}
			} else if (bsz != seg.file_size) {
				fw_release(fw);
				return false;
			}
		}
		if (at + seg.memory_size > highest) highest = at + seg.memory_size;
		fw->segments++;
	}
	fw->size = (highest + 0xfffu) & ~0xfffu;
	return fw->segments > 0;
}

static bool pas_stage_and_setup(const char *stem, uint32_t pas_id, uint32_t region_base, uint32_t region_size, const struct fw_image *fw)
{
	vm_offset_t page = 0;
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE * 4, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
	bzero((void *)page, PAGE_SIZE * 4);
	memcpy((void *)page, fw->header, fw->header_size);
	for (uint32_t off = 0; off < PAGE_SIZE * 4; off += 64) {
		uintptr_t line = page + off;
		__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
	}
	__builtin_arm_dsb(0xf);

	uint64_t res1 = 0;
	int rc = scm_pas_call(SCM_PAS_INIT_IMAGE, 2u | (2u << 6), pas_id, kvtophys(page), 0, &res1);
	if (rc != 0 || res1 != 0) {
		rc = scm_pas_call(SCM_PAS_INIT_IMAGE, 2u | (2u << 4), pas_id, kvtophys(page), 0, &res1);
	}
	kmem_free(kernel_map, page, PAGE_SIZE * 4);
	if (rc != 0 || res1 != 0) {
		say("PAS_INIT_IMAGE(pas=%#x) returned %d/%#llx\n", pas_id, rc, res1);
		return false;
	}
	rc = scm_pas_call(SCM_PAS_MEM_SETUP, 3u, pas_id, region_base, fw->size, &res1);
	if (rc != 0 || res1 != 0) {
		say("PAS_MEM_SETUP(pas=%#x) returned %d/%#llx\n", pas_id, rc, res1);
		return false;
	}
	volatile uint8_t *carveout = (volatile uint8_t *)ml_io_map_unmappable(region_base, region_size, VM_WIMG_WCOMB);
	if (carveout == NULL) return false;
	for (unsigned s = 0; s < fw->segments; s++) {
		uint32_t at = fw->segment[s].at, fsz = fw->segment[s].file_size, msz = fw->segment[s].memory_size;
		const uint8_t *src = fw->segment[s].file;
		if (src != NULL) {
			for (uint32_t i = 0; i < fsz; i++) carveout[at + i] = src[i];
		} else if (fsz != 0) {
			char bname[32];
			uint64_t bsz = 0;
			snprintf(bname, sizeof(bname), "%s.b%02u", stem, fw->segment[s].seg_idx);
			if (!annibale_ufs_fat16_fw_read(bname, &bsz, carveout + at, fsz) || bsz != fsz) {
				ml_io_unmap((vm_offset_t)carveout, region_size);
				return false;
			}
		}
		for (uint32_t i = fsz; i < msz; i++) carveout[at + i] = 0;
	}
	__builtin_arm_dsb(0xf);
	ml_io_unmap((vm_offset_t)carveout, region_size);
	return true;
}

#endif /* _ANNIBALE_ADSP_FW_INC_ */

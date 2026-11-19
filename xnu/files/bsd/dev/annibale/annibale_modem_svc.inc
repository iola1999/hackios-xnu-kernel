#ifndef _ANNIBALE_MODEM_SVC_INC_
#define _ANNIBALE_MODEM_SVC_INC_

/*
 * APPS-side QMI services required by SM8750 MPSS Root PD to mount EFS and start its Modem User PD:
 *   - RMTFS           (service 0x0e / 14, v1, inst 0): read-only UFS modemst1/2/fsg/fsc + RAM write shadow
 *   - MEMSHARE / DHMS (service 0x34 / 52, v1, inst 1): dynamic DMA allocation + TrustZone HYP_ASSIGN
 *   - SERVREG_LOCATOR (service 0x40 / 64, v1, inst 1): protection domain map from stock Redmi K90 .jsn tables
 * Also includes QMI_LOC (0x10) position indication parser (privacy-safe: never logs raw lat/lon).
 *
 * Included by bsd/dev/annibale/annibale_modem.c.
 */

#define QMI_SVC_RMTFS                   0x0eu
#define QMI_SVC_MEMSHARE                0x34u
#define QMI_SVC_SERVREG_LOC             0x40u

#define QMI_RMTFS_OPEN                  0x0001u
#define QMI_RMTFS_CLOSE                 0x0002u
#define QMI_RMTFS_RW_IOVEC              0x0003u
#define QMI_RMTFS_ALLOC_BUFF            0x0004u
#define QMI_RMTFS_GET_DEV_ERROR         0x0005u

#define QMI_MEM_ALLOC_GENERIC           0x0022u
#define QMI_MEM_FREE_GENERIC            0x0023u
#define QMI_MEM_QUERY_SIZE              0x0024u

#define QMI_SERVREG_GET_DOMAIN_LIST     0x0021u
#define QMI_SERVREG_PFR                 0x0024u

#define RMTFS_SECTOR_SIZE               512u
#define RMTFS_SHARED_SIZE               0x00400000u     /* 4 MiB (DTS rmtfs_sharedmem@0) */
#define MEMSHARE_LOWDMA_SIZE            0x00a00000u     /* 10 MiB in lowdma below RMTFS */
#define RMTFS_MAX_CALLERS               16u
#define RMTFS_PART_COUNT                4u
#define MEMSHARE_MAX_CLIENTS            7u

/* ---- WCOMB-safe memory helpers (avoid dc zva in bzero on non-cacheable mappings) -------------------- */

static void
svc_wcomb_zero(volatile uint8_t *dst, size_t len)
{
	size_t i = 0;

	while (i < len && (((uintptr_t)(dst + i)) & 7u) != 0) {
		dst[i++] = 0;
	}
	while (i + 8u <= len) {
		*(volatile uint64_t *)(uintptr_t)(dst + i) = 0;
		i += 8u;
	}
	while (i < len) {
		dst[i++] = 0;
	}
	__builtin_arm_dsb(0xf);
}

static void
svc_wcomb_write(volatile uint8_t *dst, const uint8_t *src, size_t len)
{
	size_t i = 0;

	while (i + 8u <= len && (((uintptr_t)(dst + i)) & 7u) == 0 && (((uintptr_t)(src + i)) & 7u) == 0) {
		*(volatile uint64_t *)(uintptr_t)(dst + i) = *(const uint64_t *)(uintptr_t)(src + i);
		i += 8u;
	}
	while (i < len) {
		dst[i] = src[i];
		i++;
	}
	__builtin_arm_dsb(0xf);
}

static void
svc_wcomb_read(uint8_t *dst, volatile const uint8_t *src, size_t len)
{
	size_t i = 0;

	__asm__ volatile("dmb ishld" ::: "memory");
	while (i + 8u <= len && (((uintptr_t)(src + i)) & 7u) == 0 && (((uintptr_t)(dst + i)) & 7u) == 0) {
		*(uint64_t *)(uintptr_t)(dst + i) = *(volatile const uint64_t *)(uintptr_t)(src + i);
		i += 8u;
	}
	while (i < len) {
		dst[i] = src[i];
		i++;
	}
}

/* ---- QMI TLV response encoder helpers --------------------------------------------------------------- */

static bool
svc_tlv_put(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, const void *val, uint16_t len)
{
	if (*pos + 3u + len > cap) {
		return false;
	}
	buf[*pos + 0] = type;
	buf[*pos + 1] = (uint8_t)(len & 0xffu);
	buf[*pos + 2] = (uint8_t)(len >> 8);
	if (len > 0 && val != NULL) {
		memcpy(buf + *pos + 3u, val, len);
	}
	*pos += 3u + len;
	return true;
}

static bool
svc_tlv_put_result(uint8_t *buf, size_t cap, size_t *pos, uint16_t result, uint16_t error)
{
	uint8_t v[4] = {
		(uint8_t)(result & 0xffu), (uint8_t)(result >> 8),
		(uint8_t)(error & 0xffu), (uint8_t)(error >> 8),
	};
	return svc_tlv_put(buf, cap, pos, 0x02u, v, sizeof(v));
}

static bool
svc_tlv_put_u8(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, uint8_t val)
{
	return svc_tlv_put(buf, cap, pos, type, &val, 1u);
}

static bool
svc_tlv_put_u16(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, uint16_t val)
{
	uint8_t v[2] = { (uint8_t)(val & 0xffu), (uint8_t)(val >> 8) };
	return svc_tlv_put(buf, cap, pos, type, v, sizeof(v));
}

static bool
svc_tlv_put_u32(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, uint32_t val)
{
	uint8_t v[4] = {
		(uint8_t)(val & 0xffu), (uint8_t)((val >> 8) & 0xffu),
		(uint8_t)((val >> 16) & 0xffu), (uint8_t)(val >> 24),
	};
	return svc_tlv_put(buf, cap, pos, type, v, sizeof(v));
}

static bool
svc_tlv_put_u64(uint8_t *buf, size_t cap, size_t *pos, uint8_t type, uint64_t val)
{
	uint8_t v[8];
	for (unsigned i = 0; i < 8u; i++) {
		v[i] = (uint8_t)((val >> (i * 8u)) & 0xffu);
	}
	return svc_tlv_put(buf, cap, pos, type, v, sizeof(v));
}

static void
svc_qmi_send_resp(struct annibale_qrtr_port *port, uint32_t to_node, uint32_t to_port,
    uint16_t txn_id, uint16_t msg_id, const void *tlvs, size_t tlv_len)
{
	uint8_t pkt[1024];
	size_t n;
	int err;

	if (port == NULL || tlv_len > sizeof(pkt) - 7u) {
		return;
	}
	n = annibale_qmi_encode_msg(pkt, sizeof(pkt), 0x02u, txn_id, msg_id, tlvs, (uint16_t)tlv_len);
	if (n == 0) {
		return;
	}
	err = annibale_qrtr.send(port, to_node, to_port, pkt, n);
	if (err != 0) {
		printf("annibale-modem: svc resp msg=%#x to node %u port %u failed (err=%d)\n",
		    msg_id, to_node, to_port, err);
	}
}

/* ---- Modem <4 GiB lowdma arena (below audio 0xcf66f000 & IPA 0xcf6fc000, above Wi-Fi 0xce781000) ---- */

static struct {
	bool     inited;
	uint64_t rmtfs_pa;
	uint32_t rmtfs_sz;
	vm_offset_t rmtfs_va;
	bool     rmtfs_assigned;
	uint64_t memshare_base;
	uint64_t memshare_size;
	uint64_t memshare_used;
} modem_dma_arena;

static void
modem_dma_arena_init(void)
{
	uint64_t low = 0, mb = 0;

	if (modem_dma_arena.inited) {
		return;
	}
	modem_dma_arena.inited = true;
	if (!PE_parse_boot_argn("lowdma", &low, sizeof(low)) ||
	    !PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) ||
	    low == 0 || mb < 64) {
		return;
	}
	uint64_t low_end = low + (mb << 20);
	if (low_end > 0x100000000ULL || low_end < low + 0x0752c000ULL) {
		return;
	}
	/* Reserve [arena_top - 4 MiB, arena_top) for RMTFS and up to 10 MiB below it for MEMSHARE. */
	uint64_t arena_top = (low_end - 0x0012c000ULL) & ~0xfffffULL;
	uint64_t safe_floor = (low + 0x070c0000ULL + 0xfffffULL) & ~0xfffffULL;
	if (arena_top <= safe_floor + RMTFS_SHARED_SIZE) {
		return;
	}
	modem_dma_arena.rmtfs_pa = arena_top - RMTFS_SHARED_SIZE;
	modem_dma_arena.rmtfs_sz = RMTFS_SHARED_SIZE;
	uint64_t avail = modem_dma_arena.rmtfs_pa - safe_floor;
	modem_dma_arena.memshare_size = avail > MEMSHARE_LOWDMA_SIZE ? MEMSHARE_LOWDMA_SIZE : avail;
	modem_dma_arena.memshare_base = modem_dma_arena.rmtfs_pa - modem_dma_arena.memshare_size;
	modem_dma_arena.memshare_used = 0;
	printf("annibale-modem: lowdma arena rmtfs=%#llx/%#x memshare=%#llx/%#llx\n",
	    modem_dma_arena.rmtfs_pa, modem_dma_arena.rmtfs_sz,
	    modem_dma_arena.memshare_base, modem_dma_arena.memshare_size);
}

/* ---- SERVREG_LOCATOR (service 0x40, v1, inst 1) ----------------------------------------------------- */

struct servreg_pd_entry {
	const char *service;
	const char *domain;
	uint32_t    instance;
};

/* Extracted from stock Redmi K90 modemfirmware_a.img.gz & modem_a.img.gz .jsn descriptors. */
static const struct servreg_pd_entry servreg_pd_table[] = {
	{ "tms/servreg",     "msm/modem/root_pd",   180u },
	{ "gps/gps_service", "msm/modem/root_pd",   180u },
	{ "tms/servreg",     "msm/modem/oem_pd",    180u },
	{ "oempd/srv1",      "msm/modem/oem_pd",    180u },
	{ "tms/servreg",     "msm/adsp/root_pd",     74u },
	{ "tms/servreg",     "msm/adsp/sensor_pd",   74u },
	{ "tms/servreg",     "msm/adsp/audio_pd",    74u },
	{ "avs/audio",       "msm/adsp/audio_pd",    74u },
	{ "tms/servreg",     "msm/adsp/charger_pd",  74u },
	{ "tms/servreg",     "msm/cdsp/root_pd",     76u },
};

static struct annibale_qrtr_port *svc_servreg_port;

static void
svc_servreg_rx(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	const uint8_t *buf = data;
	uint16_t txn_id, msg_id, tlv_len;
	const uint8_t *tlvs;
	uint8_t resp[768];
	size_t rpos = 0;

	(void)context;
	if (size < 7u) {
		return;
	}
	txn_id = (uint16_t)(buf[1] | (buf[2] << 8));
	msg_id = (uint16_t)(buf[3] | (buf[4] << 8));
	tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
	if ((size_t)7u + tlv_len > size) {
		return;
	}
	tlvs = buf + 7u;

	if (msg_id == QMI_SERVREG_GET_DOMAIN_LIST) {
		uint16_t slen = 0, olen = 0;
		const uint8_t *sv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &slen);
		const uint8_t *ov = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &olen);
		char svc_name[65] = { 0 };
		uint32_t domain_off = 0;

		if (sv == NULL || slen == 0) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			svc_qmi_send_resp(svc_servreg_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		size_t copy = slen < sizeof(svc_name) - 1u ? slen : sizeof(svc_name) - 1u;
		memcpy(svc_name, sv, copy);
		svc_name[copy] = '\0';
		if (ov != NULL && olen >= 4u) {
			memcpy(&domain_off, ov, 4u);
		}

		uint16_t total = 0;
		uint8_t list_buf[512];
		size_t lpos = 1u;
		uint8_t sent_cnt = 0;
		for (unsigned i = 0; i < sizeof(servreg_pd_table) / sizeof(servreg_pd_table[0]); i++) {
			if (strcmp(servreg_pd_table[i].service, svc_name) != 0) {
				continue;
			}
			if (total >= domain_off) {
				size_t dlen = strlen(servreg_pd_table[i].domain);
				if (dlen < 65u && lpos + 1u + dlen + 4u + 1u + 4u <= sizeof(list_buf)) {
					list_buf[lpos++] = (uint8_t)dlen;
					memcpy(list_buf + lpos, servreg_pd_table[i].domain, dlen);
					lpos += dlen;
					uint32_t inst = servreg_pd_table[i].instance;
					memcpy(list_buf + lpos, &inst, 4u);
					lpos += 4u;
					list_buf[lpos++] = 0u; /* service_data_valid */
					bzero(list_buf + lpos, 4u);
					lpos += 4u;
					sent_cnt++;
				}
			}
			total++;
		}
		list_buf[0] = sent_cnt;
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_tlv_put_u16(resp, sizeof(resp), &rpos, 0x10u, total);
		svc_tlv_put_u16(resp, sizeof(resp), &rpos, 0x11u, 1u); /* db_rev_count */
		if (sent_cnt > 0) {
			svc_tlv_put(resp, sizeof(resp), &rpos, 0x12u, list_buf, (uint16_t)lpos);
		}
		printf("annibale-modem: servreg_loc GET_DOMAIN_LIST svc=%s off=%u -> total=%u sent=%u\n",
		    svc_name, domain_off, total, sent_cnt);
		svc_qmi_send_resp(svc_servreg_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	} else if (msg_id == QMI_SERVREG_PFR) {
		printf("annibale-modem: servreg_loc PFR indication from node %u port %u\n", from_node, from_port);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_qmi_send_resp(svc_servreg_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	} else {
		printf("annibale-modem: servreg_loc unknown msg=%#x\n", msg_id);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
		svc_qmi_send_resp(svc_servreg_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	}
}

/* ---- MEMSHARE / DHMS (service 0x34, v1, inst 1) ----------------------------------------------------- */

struct memshare_client_slot {
	uint32_t client_id;
	uint32_t dts_size;
	bool     shared;
	bool     allocated;
	uint64_t pa;
	uint32_t alloc_sz;
	vm_offset_t kmem_va;
};

/* Matches annibale.dts.gz qcom,memshare peripheral entries and msm_memshare.ko VMID rules. */
static struct memshare_client_slot memshare_clients[MEMSHARE_MAX_CLIENTS] = {
	{ .client_id = 0u, .dts_size = 0x00000000u, .shared = false },
	{ .client_id = 1u, .dts_size = 0x00500000u, .shared = false },
	{ .client_id = 2u, .dts_size = 0x00000000u, .shared = false },
	{ .client_id = 5u, .dts_size = 0x01000000u, .shared = true  },
	{ .client_id = 6u, .dts_size = 0x00400000u, .shared = true  },
	{ .client_id = 7u, .dts_size = 0x00100000u, .shared = true  },
	{ .client_id = 8u, .dts_size = 0x00100000u, .shared = true  },
};

static struct annibale_qrtr_port *svc_memshare_port;

static struct memshare_client_slot *
memshare_find_client(uint32_t client_id)
{
	for (unsigned i = 0; i < MEMSHARE_MAX_CLIENTS; i++) {
		if (memshare_clients[i].client_id == client_id) {
			return &memshare_clients[i];
		}
	}
	return NULL;
}

static bool
memshare_alloc_backing(struct memshare_client_slot *c, uint32_t req_bytes, uint32_t align)
{
	uint32_t sz = (req_bytes + 0xfffu) & ~0xfffu;
	uint64_t pa = 0;
	vm_offset_t kva = 0;
	uint64_t res1 = 0;
	int64_t rc;

	if (sz == 0) {
		return false;
	}
	if (align < 4096u || (align & (align - 1u)) != 0 || align > 0x400000u) {
		align = 4096u;
	}
	modem_dma_arena_init();

	/* Prefer lowdma (<4 GiB, outside XNU direct map). Required for exclusive MSA (shared=false). */
	if (modem_dma_arena.memshare_size > 0) {
		uint64_t cur = modem_dma_arena.memshare_base + modem_dma_arena.memshare_used;
		uint64_t end = modem_dma_arena.memshare_base + modem_dma_arena.memshare_size;
		uint64_t aligned = (cur + align - 1ULL) & ~((uint64_t)align - 1ULL);
		if (aligned + sz > end && align > 4096u) {
			aligned = (cur + 4095ULL) & ~4095ULL;
		}
		if (aligned + sz <= end) {
			vm_offset_t tmp = ml_io_map_unmappable((vm_offset_t)aligned, (vm_size_t)sz, VM_WIMG_WCOMB);
			if (tmp != 0) {
				svc_wcomb_zero((volatile uint8_t *)tmp, sz);
				ml_io_unmap(tmp, (vm_size_t)sz);
				pa = aligned;
				modem_dma_arena.memshare_used = (aligned + sz) - modem_dma_arena.memshare_base;
			}
		}
	}

	/* Shared allocations exceeding lowdma may fall back to contiguous kernel RAM. */
	if (pa == 0 && c->shared && sz <= 0x02000000u) {
		if (kmem_alloc_contig(kernel_map, &kva, sz, PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
			uint64_t p0 = kvtophys(kva);
			bool contig = (p0 != 0 && (p0 & 0xfffULL) == 0);
			for (uint32_t off = 0; contig && off < sz; off += 4096u) {
				if (kvtophys(kva + off) != p0 + off) {
					contig = false;
				}
			}
			if (contig) {
				for (uint32_t off = 0; off < sz; off += 64u) {
					uintptr_t line = (uintptr_t)kva + off;
					__asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
				}
				__builtin_arm_dsb(0xf);
				pa = p0;
			} else {
				kmem_free(kernel_map, kva, sz);
				kva = 0;
			}
		}
	}

	if (pa == 0) {
		printf("annibale-modem: memshare cid=%u sz=%#x shared=%d no backing memory\n",
		    c->client_id, sz, c->shared ? 1 : 0);
		return false;
	}
	rc = annibale_tz_assign_mem(pa, sz, MPSS_VMID_MSA, c->shared, &res1);
	printf("annibale-modem: memshare cid=%u pa=%#llx sz=%#x shared=%d hyp_assign=(%lld,%#llx)\n",
	    c->client_id, pa, sz, c->shared ? 1 : 0, rc, res1);
	if (rc != 0 || res1 != 0) {
		if (kva != 0) {
			kmem_free(kernel_map, kva, sz);
		}
		return false;
	}
	c->allocated = true;
	c->pa = pa;
	c->alloc_sz = sz;
	c->kmem_va = kva;
	return true;
}

static void
svc_memshare_rx(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	const uint8_t *buf = data;
	uint16_t txn_id, msg_id, tlv_len;
	const uint8_t *tlvs;
	uint8_t resp[256];
	size_t rpos = 0;

	(void)context;
	if (size < 7u) {
		return;
	}
	txn_id = (uint16_t)(buf[1] | (buf[2] << 8));
	msg_id = (uint16_t)(buf[3] | (buf[4] << 8));
	tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
	if ((size_t)7u + tlv_len > size) {
		return;
	}
	tlvs = buf + 7u;

	if (msg_id == QMI_MEM_QUERY_SIZE) {
		uint16_t len = 0;
		const uint8_t *cv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &len);
		uint32_t cid = 0;
		struct memshare_client_slot *c;

		if (cv == NULL || len < 4u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		memcpy(&cid, cv, 4u);
		c = memshare_find_client(cid);
		if (c == NULL) {
			printf("annibale-modem: memshare QUERY_SIZE unknown cid=%u\n", cid);
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 3u);
		} else {
			printf("annibale-modem: memshare QUERY_SIZE cid=%u -> %#x\n", cid, c->dts_size);
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
			svc_tlv_put_u32(resp, sizeof(resp), &rpos, 0x10u, c->dts_size);
		}
		svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	} else if (msg_id == QMI_MEM_ALLOC_GENERIC) {
		uint16_t l1 = 0, l2 = 0, l4 = 0, l11 = 0;
		const uint8_t *v1 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l1);
		const uint8_t *v2 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &l2);
		const uint8_t *v4 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x04u, &l4);
		const uint8_t *v11 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x11u, &l11);
		uint32_t num_bytes = 0, cid = 0, seq_id = 0, align = 4096u;
		struct memshare_client_slot *c;

		if (v1 == NULL || l1 < 4u || v2 == NULL || l2 < 4u || v4 == NULL || l4 < 4u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		memcpy(&num_bytes, v1, 4u);
		memcpy(&cid, v2, 4u);
		memcpy(&seq_id, v4, 4u);
		if (v11 != NULL && l11 >= 4u) {
			uint32_t al = 0;
			memcpy(&al, v11, 4u);
			/* dhms_mem_block_align_enum_v01: 0=2B, 11=4KiB, ..., 21=4MiB. */
			if (al >= 11u && al <= 21u) {
				align = 1u << (al + 1u);
			}
		}
		c = memshare_find_client(cid);
		if (c == NULL) {
			printf("annibale-modem: memshare ALLOC unknown cid=%u bytes=%#x\n", cid, num_bytes);
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		if (num_bytes == 0) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
			svc_tlv_put_u32(resp, sizeof(resp), &rpos, 0x10u, seq_id);
			svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		if (!c->allocated) {
			uint32_t alloc_req = num_bytes;
			if (c->dts_size > alloc_req && c->dts_size <= 0x01000000u) {
				alloc_req = c->dts_size;
			}
			if (!memshare_alloc_backing(c, alloc_req, align)) {
				svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
				svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
				return;
			}
		}
		if (num_bytes > c->alloc_sz) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
			return;
		}
		uint8_t info[13];
		info[0] = 1u; /* dhms_mem_alloc_addr_info_len */
		memcpy(info + 1, &c->pa, 8u);
		memcpy(info + 9, &c->alloc_sz, 4u);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_tlv_put_u32(resp, sizeof(resp), &rpos, 0x10u, seq_id);
		svc_tlv_put(resp, sizeof(resp), &rpos, 0x11u, info, sizeof(info));
		svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	} else if (msg_id == QMI_MEM_FREE_GENERIC) {
		uint16_t l10 = 0;
		const uint8_t *v10 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &l10);
		uint32_t cid = 0;
		if (v10 != NULL && l10 >= 4u) {
			memcpy(&cid, v10, 4u);
		}
		/* Retain assigned slab for idempotent re-alloc by the same client_id. */
		printf("annibale-modem: memshare FREE cid=%u (slab retained)\n", cid);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	} else {
		printf("annibale-modem: memshare unknown msg=%#x\n", msg_id);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
		svc_qmi_send_resp(svc_memshare_port, from_node, from_port, txn_id, msg_id, resp, rpos);
	}
}

/* ---- RMTFS (service 0x0e, v1, inst 0): read-only UFS + RAM write shadow ----------------------------- */

struct rmtfs_part_desc {
	const char *req_path;
	const char *short_name;
	const char *ufs_part;
	uint32_t    max_bytes;
	uint8_t    *shadow;
	bool        shadow_loaded;
};

static struct rmtfs_part_desc rmtfs_parts[RMTFS_PART_COUNT] = {
	{ "/boot/modem_fs1", "modem_fs1", "modemst1", 0x00800000u, NULL, false },
	{ "/boot/modem_fs2", "modem_fs2", "modemst2", 0x00800000u, NULL, false },
	{ "/boot/modem_fsg", "modem_fsg", "fsg",      0x00800000u, NULL, false },
	{ "/boot/modem_fsc", "modem_fsc", "fsc",      0x00100000u, NULL, false },
};

struct rmtfs_caller {
	bool     open;
	uint32_t part_idx;
	uint32_t rd_ops, wr_ops;
};

static struct rmtfs_caller rmtfs_callers[RMTFS_MAX_CALLERS];
static struct annibale_qrtr_port *svc_rmtfs_port;
/* Single-threaded under qrtr_calling: keep the 4 KiB UFS bounce buffer off the kernel stack. */
static uint8_t rmtfs_bounce[4096];

static int
rmtfs_match_path(const char *path)
{
	for (unsigned i = 0; i < RMTFS_PART_COUNT; i++) {
		if (strcmp(path, rmtfs_parts[i].req_path) == 0 ||
		    strcmp(path, rmtfs_parts[i].short_name) == 0 ||
		    strcmp(path, rmtfs_parts[i].ufs_part) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static bool
rmtfs_ensure_shared_buf(void)
{
	uint64_t res1 = 0;
	int64_t rc;

	if (modem_dma_arena.rmtfs_assigned && modem_dma_arena.rmtfs_va != 0) {
		return true;
	}
	modem_dma_arena_init();
	if (modem_dma_arena.rmtfs_pa == 0 || modem_dma_arena.rmtfs_sz == 0) {
		printf("annibale-modem: rmtfs no lowdma shared buffer available\n");
		return false;
	}
	if (modem_dma_arena.rmtfs_va == 0) {
		modem_dma_arena.rmtfs_va = ml_io_map_unmappable((vm_offset_t)modem_dma_arena.rmtfs_pa,
		    (vm_size_t)modem_dma_arena.rmtfs_sz, VM_WIMG_WCOMB);
		if (modem_dma_arena.rmtfs_va == 0) {
			printf("annibale-modem: rmtfs map failed pa=%#llx\n", modem_dma_arena.rmtfs_pa);
			return false;
		}
		svc_wcomb_zero((volatile uint8_t *)modem_dma_arena.rmtfs_va, modem_dma_arena.rmtfs_sz);
	}
	rc = annibale_tz_assign_mem(modem_dma_arena.rmtfs_pa, modem_dma_arena.rmtfs_sz,
	    MPSS_VMID_MSA, true, &res1);
	printf("annibale-modem: rmtfs shared buf pa=%#llx sz=%#x hyp_assign=(%lld,%#llx)\n",
	    modem_dma_arena.rmtfs_pa, modem_dma_arena.rmtfs_sz, rc, res1);
	if (rc != 0 || res1 != 0) {
		return false;
	}
	modem_dma_arena.rmtfs_assigned = true;
	return true;
}

/* Lazily materialize a RAM shadow for writes so UFS flash partitions are NEVER modified. */
static bool
rmtfs_ensure_shadow(struct rmtfs_part_desc *p)
{
	vm_offset_t va = 0;

	if (p->shadow != NULL && p->shadow_loaded) {
		return true;
	}
	if (p->shadow == NULL) {
		if (kmem_alloc(kernel_map, &va, p->max_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		p->shadow = (uint8_t *)va;
	}
	for (uint32_t off = 0; off < p->max_bytes; off += 65536u) {
		uint32_t chunk = (p->max_bytes - off > 65536u) ? 65536u : (p->max_bytes - off);
		int err = annibale_ufs_part_read(p->ufs_part, off, p->shadow + off, chunk);
		if (err != 0) {
			if (off == 0) {
				printf("annibale-modem: rmtfs shadow preload %s failed (err=%d)\n", p->ufs_part, err);
			}
			break;
		}
	}
	p->shadow_loaded = true;
	printf("annibale-modem: rmtfs RAM write shadow armed for %s (%u KiB; UFS stays read-only)\n",
	    p->ufs_part, p->max_bytes / 1024u);
	return true;
}

static bool
rmtfs_do_iovec(struct rmtfs_caller *c, bool is_write, uint32_t sector_addr,
    uint32_t phys_offset, uint32_t num_sector)
{
	struct rmtfs_part_desc *p = &rmtfs_parts[c->part_idx];
	uint64_t byte_len = (uint64_t)num_sector * RMTFS_SECTOR_SIZE;
	uint64_t disk_off = (uint64_t)sector_addr * RMTFS_SECTOR_SIZE;
	uint64_t buf_off;

	if (!modem_dma_arena.rmtfs_assigned || modem_dma_arena.rmtfs_va == 0) {
		return false;
	}
	if (byte_len == 0) {
		return true;
	}
	if ((uint64_t)phys_offset >= modem_dma_arena.rmtfs_pa &&
	    (uint64_t)phys_offset < modem_dma_arena.rmtfs_pa + modem_dma_arena.rmtfs_sz) {
		buf_off = (uint64_t)phys_offset - modem_dma_arena.rmtfs_pa;
	} else if (phys_offset < modem_dma_arena.rmtfs_sz) {
		buf_off = (uint64_t)phys_offset;
	} else {
		return false;
	}
	if (buf_off + byte_len > modem_dma_arena.rmtfs_sz || disk_off + byte_len > p->max_bytes) {
		return false;
	}
	if (is_write) {
		if (!rmtfs_ensure_shadow(p)) {
			return false;
		}
		svc_wcomb_read(p->shadow + disk_off,
		    (volatile const uint8_t *)(modem_dma_arena.rmtfs_va + buf_off), (size_t)byte_len);
		c->wr_ops++;
		return true;
	}
	if (p->shadow != NULL && p->shadow_loaded) {
		svc_wcomb_write((volatile uint8_t *)(modem_dma_arena.rmtfs_va + buf_off),
		    p->shadow + disk_off, (size_t)byte_len);
		c->rd_ops++;
		return true;
	}
	for (uint64_t done = 0; done < byte_len; ) {
		uint64_t chunk = (byte_len - done > sizeof(rmtfs_bounce)) ? sizeof(rmtfs_bounce) : (byte_len - done);
		int err = annibale_ufs_part_read(p->ufs_part, disk_off + done, rmtfs_bounce, chunk);
		if (err != 0) {
			if (c->rd_ops < 4u && done == 0) {
				printf("annibale-modem: rmtfs UFS read %s off=%#llx err=%d\n",
				    p->ufs_part, disk_off + done, err);
			}
			bzero(rmtfs_bounce, (size_t)chunk);
		}
		svc_wcomb_write((volatile uint8_t *)(modem_dma_arena.rmtfs_va + buf_off + done),
		    rmtfs_bounce, (size_t)chunk);
		done += chunk;
	}
	c->rd_ops++;
	return true;
}

static void
svc_rmtfs_rx(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	const uint8_t *buf = data;
	uint16_t txn_id, msg_id, tlv_len;
	const uint8_t *tlvs;
	uint8_t resp[128];
	size_t rpos = 0;

	(void)context;
	if (size < 7u) {
		return;
	}
	txn_id = (uint16_t)(buf[1] | (buf[2] << 8));
	msg_id = (uint16_t)(buf[3] | (buf[4] << 8));
	tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
	if ((size_t)7u + tlv_len > size) {
		return;
	}
	tlvs = buf + 7u;

	switch (msg_id) {
	case QMI_RMTFS_OPEN: {
		uint16_t plen = 0;
		const uint8_t *pv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &plen);
		char path[64] = { 0 };
		int p_idx, slot = -1;

		if (pv == NULL || plen == 0) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			break;
		}
		size_t copy = plen < sizeof(path) - 1u ? plen : sizeof(path) - 1u;
		memcpy(path, pv, copy);
		path[copy] = '\0';
		p_idx = rmtfs_match_path(path);
		if (p_idx < 0) {
			printf("annibale-modem: rmtfs OPEN unsupported path=%s\n", path);
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		for (unsigned i = 1u; i < RMTFS_MAX_CALLERS; i++) {
			if (!rmtfs_callers[i].open) {
				slot = (int)i;
				break;
			}
		}
		if (slot < 0) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		rmtfs_callers[slot].open = true;
		rmtfs_callers[slot].part_idx = (uint32_t)p_idx;
		rmtfs_callers[slot].rd_ops = 0;
		rmtfs_callers[slot].wr_ops = 0;
		printf("annibale-modem: rmtfs OPEN %s -> %s caller=%d\n",
		    path, rmtfs_parts[p_idx].ufs_part, slot);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_tlv_put_u32(resp, sizeof(resp), &rpos, 0x10u, (uint32_t)slot);
		break;
	}
	case QMI_RMTFS_CLOSE: {
		uint16_t clen = 0;
		const uint8_t *cv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &clen);
		uint32_t cid = 0;

		if (cv == NULL || clen < 4u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			break;
		}
		memcpy(&cid, cv, 4u);
		if (cid == 0 || cid >= RMTFS_MAX_CALLERS || !rmtfs_callers[cid].open) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		printf("annibale-modem: rmtfs CLOSE caller=%u (%s rd=%u wr=%u)\n",
		    cid, rmtfs_parts[rmtfs_callers[cid].part_idx].ufs_part,
		    rmtfs_callers[cid].rd_ops, rmtfs_callers[cid].wr_ops);
		rmtfs_callers[cid].open = false;
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		break;
	}
	case QMI_RMTFS_ALLOC_BUFF: {
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *v1 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l1);
		const uint8_t *v2 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &l2);
		uint32_t cid = 0, bsz = 0;

		if (v1 == NULL || l1 < 4u || v2 == NULL || l2 < 4u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			break;
		}
		memcpy(&cid, v1, 4u);
		memcpy(&bsz, v2, 4u);
		if (!rmtfs_ensure_shared_buf() || bsz > modem_dma_arena.rmtfs_sz) {
			printf("annibale-modem: rmtfs ALLOC_BUFF caller=%u sz=%#x FAILED\n", cid, bsz);
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		printf("annibale-modem: rmtfs ALLOC_BUFF caller=%u sz=%#x -> pa=%#llx\n",
		    cid, bsz, modem_dma_arena.rmtfs_pa);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_tlv_put_u64(resp, sizeof(resp), &rpos, 0x10u, modem_dma_arena.rmtfs_pa);
		break;
	}
	case QMI_RMTFS_RW_IOVEC: {
		uint16_t l1 = 0, l2 = 0, l3 = 0;
		const uint8_t *v1 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l1);
		const uint8_t *v2 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &l2);
		const uint8_t *v3 = annibale_qmi_find_tlv(tlvs, tlv_len, 0x03u, &l3);
		uint32_t cid = 0;
		bool is_write, ok = true;

		if (v1 == NULL || l1 < 4u || v2 == NULL || l2 < 1u || v3 == NULL || l3 < 1u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			break;
		}
		memcpy(&cid, v1, 4u);
		is_write = (v2[0] != 0);
		uint8_t nent = v3[0];
		if (cid == 0 || cid >= RMTFS_MAX_CALLERS || !rmtfs_callers[cid].open ||
		    (size_t)1u + (size_t)nent * 12u > l3) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		for (uint8_t i = 0; i < nent; i++) {
			uint32_t sec = 0, poff = 0, nsec = 0;
			memcpy(&sec, v3 + 1u + i * 12u + 0u, 4u);
			memcpy(&poff, v3 + 1u + i * 12u + 4u, 4u);
			memcpy(&nsec, v3 + 1u + i * 12u + 8u, 4u);
			if (!rmtfs_do_iovec(&rmtfs_callers[cid], is_write, sec, poff, nsec)) {
				ok = false;
				break;
			}
		}
		if (rmtfs_callers[cid].rd_ops + rmtfs_callers[cid].wr_ops <= 8u || !ok) {
			printf("annibale-modem: rmtfs RW_IOVEC caller=%u %s nent=%u -> %s\n",
			    cid, is_write ? "WR(ram)" : "RD(ufs)", nent, ok ? "OK" : "ERR");
		}
		svc_tlv_put_result(resp, sizeof(resp), &rpos, ok ? 0u : 1u, ok ? 0u : 1u);
		break;
	}
	case QMI_RMTFS_GET_DEV_ERROR: {
		uint16_t clen = 0;
		const uint8_t *cv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &clen);
		uint32_t cid = 0;
		if (cv == NULL || clen < 4u) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 2u);
			break;
		}
		memcpy(&cid, cv, 4u);
		if (cid == 0 || cid >= RMTFS_MAX_CALLERS || !rmtfs_callers[cid].open) {
			svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
			break;
		}
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 0u, 0u);
		svc_tlv_put_u8(resp, sizeof(resp), &rpos, 0x10u, 0u);
		break;
	}
	default:
		printf("annibale-modem: rmtfs unknown msg=%#x\n", msg_id);
		svc_tlv_put_result(resp, sizeof(resp), &rpos, 1u, 1u);
		break;
	}
	svc_qmi_send_resp(svc_rmtfs_port, from_node, from_port, txn_id, msg_id, resp, rpos);
}

/* ---- QMI_DMS (0x02), QMI_UIM (0x0b), and QMI_LOC (0x10) diagnostic decoders ------------------------- */

static bool
svc_qmi_read_result(const uint8_t *tlvs, uint16_t tlv_len, uint16_t *res_out, uint16_t *err_out)
{
	uint16_t len = 0;
	const uint8_t *v = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &len);

	if (v == NULL || len < 4u) {
		return false;
	}
	*res_out = (uint16_t)(v[0] | ((uint16_t)v[1] << 8));
	*err_out = (uint16_t)(v[2] | ((uint16_t)v[3] << 8));
	return true;
}

static void
modem_dms_handle_rx(uint8_t qmi_flags, uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len,
    uint8_t *mode_out, bool *want_online_out, bool *requery_out)
{
	uint16_t res = 0, err = 0;

	if (qmi_flags != 0x02u) {
		return;
	}
	if (msg_id == 0x002du /* QMI_DMS_GET_OPERATING_MODE */) {
		uint16_t l_mode = 0, l_reason = 0, l_hw = 0;
		const uint8_t *v_mode = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l_mode);
		const uint8_t *v_reason = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &l_reason);
		const uint8_t *v_hw = annibale_qmi_find_tlv(tlvs, tlv_len, 0x11u, &l_hw);
		uint16_t reason = 0;
		uint8_t hw = 0;

		if (v_mode == NULL || l_mode < 1u) {
			if (svc_qmi_read_result(tlvs, tlv_len, &res, &err)) {
				printf("annibale-modem: QMI DMS GET_OPERATING_MODE failed res=%u err=%u\n", res, err);
			}
			return;
		}
		if (v_reason != NULL && l_reason >= 2u) {
			reason = (uint16_t)(v_reason[0] | ((uint16_t)v_reason[1] << 8));
		}
		if (v_hw != NULL && l_hw >= 1u) {
			hw = v_hw[0];
		}
		printf("annibale-modem: QMI DMS operating_mode=%u offline_reason=%#x hw_restr=%u (device_serial=***)\n",
		    v_mode[0], reason, hw);
		*mode_out = v_mode[0];
		*want_online_out = (v_mode[0] != 0u);
	} else if (msg_id == 0x002eu /* QMI_DMS_SET_OPERATING_MODE */) {
		if (svc_qmi_read_result(tlvs, tlv_len, &res, &err)) {
			printf("annibale-modem: QMI DMS SET_OPERATING_MODE result=%u err=%u\n", res, err);
			*requery_out = true;
		}
	}
}

static void
modem_uim_handle_rx(uint8_t qmi_flags, uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len,
    uint32_t *sim_state_out)
{
	uint16_t res = 0, err = 0, clen = 0;
	const uint8_t *cv;

	if (msg_id != 0x002fu /* GET_CARD_STATUS */ && msg_id != 0x0032u /* CARD_STATUS_IND */) {
		return;
	}
	if (qmi_flags == 0x02u && svc_qmi_read_result(tlvs, tlv_len, &res, &err) && res != 0) {
		printf("annibale-modem: QMI UIM GET_CARD_STATUS failed res=%u err=%u\n", res, err);
		return;
	}
	cv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &clen);
	if (cv == NULL || clen < 15u || cv[8] == 0) {
		return;
	}
	uint8_t num_slots = cv[8], card = cv[9], upin = cv[10], uim_err = cv[13], apps = cv[14];
	uint8_t app_state = (clen >= 17u && apps > 0) ? cv[16] : 0u;
	*sim_state_out = (card == 1u) ? (app_state == 7u ? 2u : 1u) : 0u;
	printf("annibale-modem: QMI UIM slots=%u slot0=(card=%u upin=%u err=%u apps=%u app_st=%u) sim=%u (subscriber_id=***)\n",
	    num_slots, card, upin, uim_err, apps, app_state, *sim_state_out);
}

static void
modem_wms_handle_rx(uint8_t qmi_flags, uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len,
    uint32_t *sms_rx_out, uint32_t *sms_tx_out)
{
	if (msg_id == 0x0001u /* QMI_WMS_EVENT_REPORT */ && qmi_flags == 0x04u) {
		(*sms_rx_out)++;
		printf("annibale-modem: QMI WMS incoming SMS indication (total_rx=%u, sender=***)\n",
		    *sms_rx_out);
	} else if (msg_id == 0x0001u && qmi_flags == 0x02u) {
		uint16_t res = 0, err = 0;
		svc_qmi_read_result(tlvs, tlv_len, &res, &err);
		printf("annibale-modem: QMI WMS SET_EVENT_REPORT rsp res=%u err=%u\n", res, err);
	} else if (msg_id == 0x0020u /* QMI_WMS_RAW_SEND */ && qmi_flags == 0x02u) {
		(*sms_tx_out)++;
	}
}

static void
modem_voice_handle_rx(uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len, uint32_t *call_state_out)
{
	if (msg_id == 0x002eu || msg_id == 0x0020u || msg_id == 0x0021u || msg_id == 0x0022u) {
		uint16_t len = 0;
		const uint8_t *v = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &len);
		if (msg_id == 0x0021u) {
			*call_state_out = 0;
		} else if (v != NULL && len >= 3u) {
			*call_state_out = v[2];
		}
		printf("annibale-modem: QMI Voice msg=0x%04x call_state=%u peer=***\n",
		    msg_id, *call_state_out);
	}
}

static void
modem_loc_handle_rx(uint8_t qmi_flags, uint16_t txn_id, uint16_t msg_id, const uint8_t *tlvs,
    uint16_t tlv_len, bool *reg_ok_out, bool *retry_min_mask_out)
{
	if (qmi_flags == 0x02u /* QMI_RESPONSE */) {
		uint16_t res = 0, err = 0;
		if (svc_qmi_read_result(tlvs, tlv_len, &res, &err)) {
			printf("annibale-modem: QMI LOC rsp txn=%u msg=%#04x len=%u result=%u err=%u\n",
			    txn_id, msg_id, tlv_len, res, err);
			if (msg_id == 0x001eu && res == 0) {
				uint16_t ml = 0;
				const uint8_t *mv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &ml);
				uint64_t mask0 = 0;
				if (mv != NULL && ml > 0) {
					memcpy(&mask0, mv, ml < 8u ? ml : 8u);
				}
				printf("annibale-modem: QMI LOC supported_msgs len=%u w0=%#llx\n", ml, mask0);
			} else if (msg_id == 0x0020u && res == 0) {
				uint16_t rl = 0;
				const uint8_t *rv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &rl);
				uint32_t rev = 0;
				if (rv != NULL && rl >= 4u) {
					memcpy(&rev, rv, 4u);
					printf("annibale-modem: QMI LOC service_rev=%u\n", rev);
				}
			} else if (msg_id == 0x0021u) {
				if (res == 0 && reg_ok_out != NULL) {
					*reg_ok_out = true;
				} else if (res != 0 && retry_min_mask_out != NULL) {
					*retry_min_mask_out = true;
				}
			}
		}
		return;
	}
	if (qmi_flags != 0x04u /* QMI_INDICATION */) {
		return;
	}
	switch (msg_id) {
	case 0x0020u: { /* QMI_LOC_GET_SERVICE_REVISION_IND */
		uint16_t rl = 0;
		const uint8_t *rv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &rl);
		uint32_t rev = 0;
		if (rv != NULL && rl >= 4u) {
			memcpy(&rev, rv, 4u);
		}
		printf("annibale-modem: QMI LOC service_rev_ind=%u\n", rev);
		break;
	}
	case 0x0024u: { /* QMI_LOC_EVENT_POSITION_REPORT_IND */
		uint16_t l_st = 0, l_sid = 0, l_lat = 0, l_lon = 0, l_tech = 0, l_fix = 0, l_sv = 0;
		const uint8_t *v_st = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l_st);
		const uint8_t *v_sid = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &l_sid);
		const uint8_t *v_lat = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &l_lat);
		const uint8_t *v_lon = annibale_qmi_find_tlv(tlvs, tlv_len, 0x11u, &l_lon);
		const uint8_t *v_tech = annibale_qmi_find_tlv(tlvs, tlv_len, 0x23u, &l_tech);
		const uint8_t *v_fix = annibale_qmi_find_tlv(tlvs, tlv_len, 0x2bu, &l_fix);
		const uint8_t *v_sv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x2cu, &l_sv);
		uint32_t st = 0, tech = 0, fix_cnt = 0;
		uint8_t sid = 0, sv_used = 0;

		if (v_st != NULL && l_st >= 4u) memcpy(&st, v_st, 4u);
		if (v_sid != NULL && l_sid >= 1u) sid = v_sid[0];
		if (v_tech != NULL && l_tech >= 4u) memcpy(&tech, v_tech, 4u);
		if (v_fix != NULL && l_fix >= 4u) memcpy(&fix_cnt, v_fix, 4u);
		if (v_sv != NULL && l_sv >= 1u) sv_used = v_sv[0];
		/* Privacy contract: never log raw latitude/longitude coordinates. */
		printf("annibale-modem: QMI LOC pos_ind status=%u sid=%u has_latlon=%d tech=%#x fixes=%u sv_used=%u\n",
		    st, sid, (v_lat != NULL && l_lat == 8u && v_lon != NULL && l_lon == 8u) ? 1 : 0,
		    tech, fix_cnt, sv_used);
		break;
	}
	case 0x0025u: { /* QMI_LOC_EVENT_GNSS_SV_INFO_IND */
		uint16_t l_sv = 0;
		const uint8_t *v_sv = annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &l_sv);
		uint8_t count = (v_sv != NULL && l_sv >= 1u) ? v_sv[0] : 0u;
		printf("annibale-modem: QMI LOC sv_info count=%u\n", count);
		break;
	}
	case 0x0026u: { /* QMI_LOC_EVENT_NMEA_IND: log sentence tag only, never coordinates. */
		uint16_t l_nmea = 0;
		const uint8_t *v_nmea = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l_nmea);
		char tag[9] = { 0 };
		if (v_nmea != NULL && l_nmea > 0) {
			unsigned n = 0;
			while (n < 8u && n < l_nmea && v_nmea[n] != ',' && v_nmea[n] >= 0x20 && v_nmea[n] < 0x7f) {
				tag[n] = (char)v_nmea[n];
				n++;
			}
		}
		printf("annibale-modem: QMI LOC nmea tag=%s\n", tag[0] ? tag : "none");
		break;
	}
	case 0x002bu: { /* QMI_LOC_EVENT_ENGINE_STATE_IND */
		uint16_t l_eng = 0;
		const uint8_t *v_eng = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l_eng);
		int32_t eng = 0;
		if (v_eng != NULL && l_eng >= 4u) memcpy(&eng, v_eng, 4u);
		printf("annibale-modem: QMI LOC engine_state=%d\n", eng);
		break;
	}
	case 0x002cu: { /* QMI_LOC_EVENT_FIX_SESSION_STATE_IND */
		uint16_t l_ss = 0;
		const uint8_t *v_ss = annibale_qmi_find_tlv(tlvs, tlv_len, 0x01u, &l_ss);
		uint32_t ss = 0;
		if (v_ss != NULL && l_ss >= 4u) memcpy(&ss, v_ss, 4u);
		printf("annibale-modem: QMI LOC fix_session_state=%u\n", ss);
		break;
	}
	default:
		break;
	}
}

/* ---- Register local APPS QMI services on QRTR before MPSS boot -------------------------------------- */

static void
modem_start_local_services(void)
{
	if (svc_rmtfs_port == NULL) {
		svc_rmtfs_port = annibale_qrtr.open(svc_rmtfs_rx, NULL, NULL);
		if (svc_rmtfs_port != NULL) {
			(void)annibale_qrtr_publish(svc_rmtfs_port, QMI_SVC_RMTFS, 1u, 0u);
		}
	}
	if (svc_memshare_port == NULL) {
		svc_memshare_port = annibale_qrtr.open(svc_memshare_rx, NULL, NULL);
		if (svc_memshare_port != NULL) {
			(void)annibale_qrtr_publish(svc_memshare_port, QMI_SVC_MEMSHARE, 1u, 1u);
		}
	}
	if (svc_servreg_port == NULL) {
		svc_servreg_port = annibale_qrtr.open(svc_servreg_rx, NULL, NULL);
		if (svc_servreg_port != NULL) {
			(void)annibale_qrtr_publish(svc_servreg_port, QMI_SVC_SERVREG_LOC, 1u, 1u);
		}
	}
}

#endif /* _ANNIBALE_MODEM_SVC_INC_ */

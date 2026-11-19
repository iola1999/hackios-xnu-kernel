#ifndef _ANNIBALE_MODEM_BOOT_INC_
#define _ANNIBALE_MODEM_BOOT_INC_

/*
 * SM8750 MPSS SMEM partition scanner, SMP2P handshake (items 428/435),
 * crash-reason reader (item 421, digit-sanitized), and TrustZone PAS boot sequence.
 * Included by bsd/dev/annibale/annibale_modem.c.
 */

struct modem_smp2p_item {
	uint32_t magic;
	uint32_t version;
	uint16_t local;
	uint16_t remote;
	uint16_t total;
	uint16_t valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};

static void
smem_read_bytes(uint32_t byte_off, void *dst, uint32_t len)
{
	volatile const uint8_t *src = (volatile const uint8_t *)smem_base;

	if (smem_base == NULL || (uint64_t)byte_off + len > SM8750_SMEM_SIZE) {
		return;
	}
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)dst)[i] = src[byte_off + i];
	}
	__asm__ volatile("dmb ishld" ::: "memory");
}

static uint32_t
smem_find_toc_offset(void)
{
	uint32_t off = SM8750_SMEM_SIZE - 0x1000u;

	if (smem_rd(off) == 0x434f5424u) {
		return off;
	}
	if (smem_rd(0u) == 0x434f5424u) {
		return 0u;
	}
	off = smem_rd(0x0cu) + smem_rd(0x10u);
	if (off != 0 && off + 32u <= SM8750_SMEM_SIZE && smem_rd(off) == 0x434f5424u) {
		return off;
	}
	return SM8750_SMEM_SIZE;
}

static uint32_t
smem_find_partition(uint16_t host_a, uint16_t host_b, uint32_t *size_out, uint32_t *cacheline_out)
{
	uint32_t toc_off, count;

	if (smem_base == NULL) {
		return 0;
	}
	toc_off = smem_find_toc_offset();
	if (toc_off >= SM8750_SMEM_SIZE || toc_off + 32u > SM8750_SMEM_SIZE) {
		return 0;
	}
	count = smem_rd(toc_off + 8u);
	for (uint32_t i = 0; i < count && i < 64u; i++) {
		uint32_t e = toc_off + 32u + i * 48u;
		uint32_t off, sz, hosts, cacheline;
		uint16_t h0, h1;

		if (e + 48u > SM8750_SMEM_SIZE) {
			break;
		}
		off = smem_rd(e + 0u);
		sz = smem_rd(e + 4u);
		hosts = smem_rd(e + 12u);
		cacheline = smem_rd(e + 16u);
		h0 = (uint16_t)(hosts & 0xffffu);
		h1 = (uint16_t)(hosts >> 16);

		if (((h0 == host_a && h1 == host_b) || (h0 == host_b && h1 == host_a)) &&
		    off != 0 && sz >= 64u && (uint64_t)off + sz <= SM8750_SMEM_SIZE &&
		    smem_rd(off) == 0x54525024u) {
			if (size_out != NULL) {
				*size_out = sz;
			}
			if (cacheline_out != NULL) {
				*cacheline_out = cacheline;
			}
			return off;
		}
	}
	return 0;
}

static uint32_t
smem_find_item_in_partition(uint16_t host_a, uint16_t host_b, uint32_t item_id, uint32_t *size_out)
{
	uint32_t part_sz = 0, cacheline = 0;
	uint32_t part = smem_find_partition(host_a, host_b, &part_sz, &cacheline);
	uint32_t uncached_end, cached_start;

	if (part == 0) {
		return 0;
	}
	uncached_end = smem_rd(part + 12u);
	cached_start = smem_rd(part + 16u);
	if (uncached_end > part_sz || cached_start > part_sz || uncached_end > cached_start) {
		return 0;
	}
	for (uint32_t at = 32u; at + 16u <= uncached_end; ) {
		uint32_t hdr = smem_rd(part + at);
		uint32_t sz = smem_rd(part + at + 4u);
		uint32_t pad = smem_rd(part + at + 8u);
		uint32_t pad_data = pad & 0xffffu;
		uint32_t pad_hdr = pad >> 16;

		if ((hdr & 0xffffu) != SMEM_CANARY || pad_data > sz || sz > part_sz ||
		    (uint64_t)at + 16u + pad_hdr + sz > uncached_end) {
			break;
		}
		if ((hdr >> 16) == item_id) {
			if (size_out != NULL) {
				*size_out = sz - pad_data;
			}
			return part + at + 16u + pad_hdr;
		}
		at += 16u + pad_hdr + sz;
	}
	if (cacheline != 0 && cacheline <= part_sz) {
		for (uint32_t at = part_sz - cacheline; at >= cached_start && at < part_sz; ) {
			uint32_t hdr = smem_rd(part + at);
			uint32_t sz = smem_rd(part + at + 4u);
			uint32_t pad = smem_rd(part + at + 8u);
			uint32_t pad_data = pad & 0xffffu;
			uint32_t pad_hdr = pad >> 16;
			uint64_t sub_off = (uint64_t)pad_hdr + sz;
			uint64_t decr = sub_off + cacheline;

			if ((hdr & 0xffffu) != SMEM_CANARY || pad_data > sz ||
			    sz > at - cached_start || at < sub_off) {
				break;
			}
			if (at - (uint32_t)sub_off < cached_start) {
				break;
			}
			if ((hdr >> 16) == item_id) {
				if (size_out != NULL) {
					*size_out = sz - pad_data;
				}
				return part + at - (uint32_t)sub_off;
			}
			if (at < decr) {
				break;
			}
			at -= (uint32_t)decr;
		}
	}
	return 0;
}

extern int64_t annibale_tz_pas_shutdown(uint32_t pas_id, uint64_t *res1_out);

static volatile bool mpss_pas_dtb_authed, mpss_pas_fw_authed, mpss_stopping;

static uint32_t
smem_find_mpss_item(uint32_t item_id, uint32_t *size_out)
{
	/* Never fall back to SMEM_HOST_GLOBAL for MPSS GLINK/SMP2P items (avoids ADSP queue aliasing). */
	return smem_find_item_in_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, item_id, size_out);
}

static uint32_t
smem_alloc_mpss_item(uint32_t item_id, uint32_t req_size)
{
	uint32_t existing_sz = 0;
	uint32_t off = smem_find_item_in_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, item_id, &existing_sz);
	uint32_t part_sz = 0, part, uncached_end, cached_start, aligned_sz, pad_data;

	if (off != 0) {
		return existing_sz >= req_size ? off : 0;
	}
	lck_mtx_lock(&annibale_smem_alloc_lock);
	part = smem_find_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, &part_sz, NULL);
	if (part != 0) {
		uncached_end = smem_rd(part + 12u);
		cached_start = smem_rd(part + 16u);
		aligned_sz = (req_size + 7u) & ~7u;
		pad_data = aligned_sz - req_size;
		if (uncached_end >= 32u && uncached_end + 16u + aligned_sz <= cached_start) {
			off = part + uncached_end + 16u;
			for (uint32_t i = 0; i < aligned_sz; i += 4u) {
				smem_wr(off + i, 0);
			}
			smem_wr(part + uncached_end, SMEM_CANARY | (item_id << 16));
			smem_wr(part + uncached_end + 4u, aligned_sz);
			smem_wr(part + uncached_end + 8u, pad_data);
			smem_wr(part + uncached_end + 12u, 0);
			smem_wr(part + 12u, uncached_end + 16u + aligned_sz);
		}
	}
	lck_mtx_unlock(&annibale_smem_alloc_lock);
	return off;
}

static void
ipcc_ring_mpss(uint32_t signal)
{
	if (ipcc_base != 0) {
		mmio_wr(ipcc_base + IPCC_REG_SEND, (IPCC_CLIENT_MPSS << 16) | (signal & 0xffffu));
	}
}

static bool
mpss_glink_write_raw(uint16_t cmd, uint16_t p1, uint32_t p2, const void *payload, uint32_t len)
{
	uint32_t aligned = (8u + len + 7u) & ~7u;
	uint32_t head, tail, free_bytes, pos;
	uint8_t hdr[8];

	if (mpss_glink.desc_off == 0 || mpss_glink.our_q_off == 0 || mpss_glink.our_q_sz < aligned + 8u) {
		return false;
	}
	tail = smem_rd(mpss_glink.desc_off + 0u);
	head = smem_rd(mpss_glink.desc_off + 4u);
	if (head >= mpss_glink.our_q_sz || tail >= mpss_glink.our_q_sz) {
		return false;
	}
	free_bytes = (tail <= head) ? (mpss_glink.our_q_sz - head + tail) : (tail - head);
	if (free_bytes < aligned + 8u) {
		return false;
	}
	hdr[0] = (uint8_t)(cmd & 0xffu);
	hdr[1] = (uint8_t)(cmd >> 8);
	hdr[2] = (uint8_t)(p1 & 0xffu);
	hdr[3] = (uint8_t)(p1 >> 8);
	memcpy(hdr + 4, &p2, 4u);

	pos = head;
	for (uint32_t i = 0; i < aligned; i += 4u) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4u; b++) {
			uint32_t idx = i + b;
			uint8_t byte = idx < 8u ? hdr[idx] :
			    (payload != NULL && idx - 8u < len ? ((const uint8_t *)payload)[idx - 8u] : 0u);
			w |= (uint32_t)byte << (b * 8u);
		}
		smem_wr(mpss_glink.our_q_off + pos, w);
		pos = (pos + 4u == mpss_glink.our_q_sz) ? 0u : pos + 4u;
	}
	smem_wr(mpss_glink.desc_off + 4u, pos);
	(void)smem_rd(mpss_glink.desc_off + 4u);
	ipcc_ring_mpss(IPCC_SIG_GLINK);
	mpss_glink.tx_count++;
	return true;
}

static void
mpss_glink_add_intent(uint32_t id, uint32_t size)
{
	int free_idx = -1;

	if (id == 0 || size == 0) {
		return;
	}
	for (unsigned i = 0; i < MPSS_GLINK_MAX_INTENTS; i++) {
		if (mpss_glink.their_intents[i].valid && mpss_glink.their_intents[i].id == id) {
			mpss_glink.their_intents[i].size = size;
			mpss_glink.their_intents[i].busy = false;
			return;
		}
		if (!mpss_glink.their_intents[i].valid && free_idx < 0) {
			free_idx = (int)i;
		}
	}
	if (free_idx >= 0) {
		mpss_glink.their_intents[free_idx].id = id;
		mpss_glink.their_intents[free_idx].size = size;
		mpss_glink.their_intents[free_idx].valid = true;
		mpss_glink.their_intents[free_idx].busy = false;
		mpss_glink.their_intent_count++;
	}
}

static bool
mpss_glink_take_intent(uint32_t min_size, uint32_t *iid_out)
{
	int best = -1;

	for (unsigned i = 0; i < MPSS_GLINK_MAX_INTENTS; i++) {
		if (!mpss_glink.their_intents[i].valid || mpss_glink.their_intents[i].busy ||
		    mpss_glink.their_intents[i].size < min_size) {
			continue;
		}
		if (best < 0 || mpss_glink.their_intents[i].size < mpss_glink.their_intents[best].size) {
			best = (int)i;
		}
	}
	if (best < 0) {
		return false;
	}
	mpss_glink.their_intents[best].busy = true;
	*iid_out = mpss_glink.their_intents[best].id;
	return true;
}

static void
mpss_glink_complete_intent(uint32_t iid, bool reuse)
{
	for (unsigned i = 0; i < MPSS_GLINK_MAX_INTENTS; i++) {
		if (mpss_glink.their_intents[i].valid && mpss_glink.their_intents[i].id == iid) {
			mpss_glink.their_intents[i].busy = false;
			if (!reuse) {
				mpss_glink.their_intents[i].valid = false;
				if (mpss_glink.their_intent_count > 0) {
					mpss_glink.their_intent_count--;
				}
			}
			return;
		}
	}
}

static bool
mpss_glink_has_free_intent(uint32_t min_size)
{
	for (unsigned i = 0; i < MPSS_GLINK_MAX_INTENTS; i++) {
		if (mpss_glink.their_intents[i].valid && !mpss_glink.their_intents[i].busy &&
		    mpss_glink.their_intents[i].size >= min_size) {
			return true;
		}
	}
	return false;
}

static void
mpss_glink_init_preboot(void)
{
	uint32_t desc = smem_alloc_mpss_item(ITEM_GLINK_DESCRIPTOR, 32u);
	uint32_t our_q = smem_alloc_mpss_item(ITEM_GLINK_OUR_QUEUE, GLINK_QUEUE_SIZE);

	if (desc == 0 || our_q == 0) {
		return;
	}
	lck_mtx_lock(&mpss_glink_lock);
	bzero(&mpss_glink, sizeof(mpss_glink));
	mpss_glink.desc_off = desc;
	mpss_glink.our_q_off = our_q;
	mpss_glink.our_q_sz = GLINK_QUEUE_SIZE;
	mpss_glink.our_features = GLINK_FEATURE_INTENT_REUSE;
	/* Match qcom_glink_smem.ko: leave tx_head=0 pre-boot; send VERSION after SMP2P READY & their_q exist. */
	for (uint32_t off = 0; off < 32u; off += 4u) {
		smem_wr(desc + off, 0u);
	}
	(void)smem_rd(desc + 12u);
	lck_mtx_unlock(&mpss_glink_lock);
}

static bool
mpss_glink_locate_queues(void)
{
	uint32_t d_sz = 0, o_sz = 0, t_sz = 0;
	bool was_ready = mpss_glink.ready;

	if (was_ready) {
		return true;
	}
	mpss_glink.desc_off = smem_find_mpss_item(ITEM_GLINK_DESCRIPTOR, &d_sz);
	mpss_glink.our_q_off = smem_find_mpss_item(ITEM_GLINK_OUR_QUEUE, &o_sz);
	mpss_glink.their_q_off = smem_find_mpss_item(ITEM_GLINK_THEIR_QUEUE, &t_sz);
	if (mpss_glink.desc_off == 0) {
		mpss_glink.desc_off = smem_alloc_mpss_item(ITEM_GLINK_DESCRIPTOR, 32u);
		d_sz = 32u;
	}
	if (mpss_glink.our_q_off == 0) {
		mpss_glink.our_q_off = smem_alloc_mpss_item(ITEM_GLINK_OUR_QUEUE, GLINK_QUEUE_SIZE);
		o_sz = GLINK_QUEUE_SIZE;
	}
	mpss_glink.our_q_sz = o_sz;
	mpss_glink.their_q_sz = t_sz;
	mpss_glink.ready = (mpss_glink.desc_off != 0 && d_sz >= 16u &&
	    mpss_glink.our_q_off != 0 && o_sz >= 1024u &&
	    mpss_glink.their_q_off != 0 && t_sz >= 1024u);
	if (!was_ready && mpss_glink.ready) {
		uint32_t p0 = smem_rd(mpss_glink.desc_off + 0u);
		uint32_t p1 = smem_rd(mpss_glink.desc_off + 4u);
		uint32_t p2 = smem_rd(mpss_glink.desc_off + 8u);
		uint32_t p3 = smem_rd(mpss_glink.desc_off + 12u);
		printf("annibale-modem: SMEM GLINK queues ready (desc=%#x our=%#x/%u their=%#x/%u ptrs=%u/%u/%u/%u)\n",
		    mpss_glink.desc_off, mpss_glink.our_q_off, mpss_glink.our_q_sz,
		    mpss_glink.their_q_off, mpss_glink.their_q_sz, p0, p1, p2, p3);
		if (!mpss_glink.version_sent) {
			smem_wr(mpss_glink.desc_off + 4u, 0u);
			smem_wr(mpss_glink.desc_off + 8u, 0u);
			(void)smem_rd(mpss_glink.desc_off + 8u);
			mpss_glink.version_sent = mpss_glink_write_raw(GLINK_CMD_VERSION, 1u,
			    mpss_glink.our_features, NULL, 0);
		} else {
			ipcc_ring_mpss(IPCC_SIG_GLINK);
		}
	}
	return mpss_glink.ready;
}

static void
mpss_glink_queue_intents(void)
{
	static const uint32_t intent_sizes[10] = {
		0x800, 0x800, 0x800, 0x800, 0x800,
		0x2000, 0x2000, 0x2000, 0x4400, 0x4400
	};
	uint32_t pairs[20];

	if (mpss_glink.intents_sent) {
		return;
	}
	mpss_glink.intents_sent = true;
	for (unsigned i = 0; i < 10; i++) {
		pairs[2 * i] = intent_sizes[i];
		pairs[2 * i + 1] = ++mpss_glink.next_our_iid;
	}
	mpss_glink_write_raw(GLINK_CMD_INTENT, GLINK_OUR_CID, 10u, pairs, sizeof(pairs));
}

static void
smp2p_mpss_init_ours(void)
{
	uint32_t at = smem_alloc_mpss_item(ITEM_SMP2P_MPSS_OURS, sizeof(struct modem_smp2p_item));
	struct modem_smp2p_item ours;
	char name[16] = "master-kernel";

	if (at == 0) {
		return;
	}
	smem_read_bytes(at, &ours, sizeof(ours));
	if (ours.magic == SMP2P_MAGIC) {
		return;
	}
	smem_wr(at + 0u, SMP2P_MAGIC);
	/* Match smp2p.ko: byte 4 = version (1), bytes 5..7 = features (SSR_ACK=1), +16 flags = 0. */
	smem_wr(at + 4u, SMP2P_VERSION | (SMP2P_SSR_ACK << 8));
	smem_wr(at + 8u, (uint32_t)SMEM_HOST_APPS | ((uint32_t)SMEM_HOST_MPSS << 16));
	smem_wr(at + 12u, 16u | (1u << 16));
	smem_wr(at + 16u, 0u);
	for (unsigned w = 0; w < 4u; w++) {
		uint32_t nw = 0;
		memcpy(&nw, name + 4u * w, 4u);
		smem_wr(at + 20u + 4u * w, nw);
	}
	smem_wr(at + 36u, 0u);
	ipcc_ring_mpss(IPCC_SIG_SMP2P);
}

static void
smp2p_mpss_set_stop(void)
{
	uint32_t sz = 0, at = smem_find_mpss_item(ITEM_SMP2P_MPSS_OURS, &sz);

	if (at == 0 || sz < sizeof(struct modem_smp2p_item)) {
		return;
	}
	/* annibale.dts modem_pas qcom,smem-states = <modem_smp2p_out 0> ("stop" = bit 0 of entry[0]). */
	uint32_t val = smem_rd(at + 36u);
	smem_wr(at + 36u, val | 1u);
	(void)smem_rd(at + 36u);
	ipcc_ring_mpss(IPCC_SIG_SMP2P);
}

static bool
smp2p_mpss_read_slave(uint32_t *val_out, uint32_t *flags_out)
{
	uint32_t sz = 0, at = smem_find_mpss_item(ITEM_SMP2P_MPSS_THEIRS, &sz);
	struct modem_smp2p_item item;

	if (at == 0 || sz < sizeof(item)) {
		return false;
	}
	smem_read_bytes(at, &item, sizeof(item));
	if (item.magic != SMP2P_MAGIC) {
		return false;
	}
	for (unsigned i = 0; i < item.valid && i < 16u; i++) {
		if (strncmp(item.entry[i].name, "slave-kernel", 16) == 0) {
			if (val_out != NULL) {
				*val_out = item.entry[i].value;
			}
			if (flags_out != NULL) {
				*flags_out = item.flags;
			}
			return true;
		}
	}
	return false;
}

static void
modem_sanitize_crash_str(const char *raw, uint32_t raw_len, char *out, size_t out_cap)
{
	size_t w = 0, i = 0;

	while (i < raw_len && w + 1 < out_cap) {
		unsigned char c = (unsigned char)raw[i];
		if (c == '\0') {
			break;
		}
		if (c >= '0' && c <= '9') {
			size_t j = i;
			while (j < raw_len && (unsigned char)raw[j] >= '0' && (unsigned char)raw[j] <= '9') {
				j++;
			}
			if (j - i >= 5u) {
				for (unsigned k = 0; k < 3u && w + 1 < out_cap; k++) {
					out[w++] = '*';
				}
				i = j;
				continue;
			}
			while (i < j && w + 1 < out_cap) {
				out[w++] = raw[i++];
			}
			continue;
		}
		out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
		i++;
	}
	out[w] = '\0';
}

static void
modem_print_sanitized_chunks(const char *tag, const char *raw, uint32_t raw_len, bool sanitize_digits)
{
	char clean[1025];
	char scrub[1025];
	uint32_t copy_len = raw_len > 1024u ? 1024u : raw_len;
	bool has_ascii = false;
	const char *out_str;

	if (copy_len == 0) {
		return;
	}
	for (uint32_t i = 0; i < copy_len; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (c >= 0x20 && c < 0x7f) {
			has_ascii = true;
			scrub[i] = (char)c;
		} else {
			scrub[i] = ' ';
		}
	}
	scrub[copy_len] = '\0';
	if (!has_ascii) {
		return;
	}
	if (sanitize_digits) {
		modem_sanitize_crash_str(scrub, copy_len, clean, sizeof(clean));
		out_str = clean;
	} else {
		out_str = scrub;
	}
	size_t out_len = strlen(out_str);
	for (size_t off = 0; off < out_len; ) {
		char chunk[161];
		size_t n = out_len - off > 160u ? 160u : out_len - off;
		memcpy(chunk, out_str + off, n);
		chunk[n] = '\0';
		printf("annibale-modem: %s: %s\n", tag, chunk);
		off += n;
	}
}

static void
modem_log_ring_tail(const char *tag, volatile const uint8_t *ring, uint32_t ring_len,
    uint32_t cur, bool wrapped, bool sanitize_digits)
{
	char raw[1024];
	uint32_t avail = wrapped ? ring_len : cur;
	uint32_t take = avail < sizeof(raw) ? avail : (uint32_t)sizeof(raw);
	uint32_t start;

	if (take == 0 || ring_len == 0) {
		return;
	}
	start = (cur + ring_len - take) % ring_len;
	for (uint32_t i = 0; i < take; i++) {
		raw[i] = (char)ring[(start + i) % ring_len];
	}
	modem_print_sanitized_chunks(tag, raw, take, sanitize_digits);
}

static void
modem_log_tz_diag(void)
{
	const uint64_t imem_phys = 0x14680720ULL;
	const uint64_t imem_page_pa = imem_phys & ~(uint64_t)PAGE_MASK;
	const uint64_t imem_page_off = imem_phys & (uint64_t)PAGE_MASK;
	const vm_size_t imem_map_sz = (vm_size_t)((imem_page_off + 0x420ULL + PAGE_MASK) & ~(uint64_t)PAGE_MASK);
	vm_offset_t imem_map = ml_io_map_unmappable((vm_offset_t)imem_page_pa, imem_map_sz, VM_WIMG_IO);

	if (imem_map == 0) {
		printf("annibale-modem: tzlog map failed\n");
		return;
	}
	vm_offset_t imem = imem_map + (vm_offset_t)imem_page_off;
	__asm__ volatile("dmb ishld" ::: "memory");
	uint32_t tz_diag_pa = *(volatile const uint32_t *)(imem + 0x00u);
	uint32_t hyp_pa = *(volatile const uint32_t *)(imem + 0x410u);
	uint32_t hyp_sz = *(volatile const uint32_t *)(imem + 0x414u);
	ml_io_unmap(imem_map, imem_map_sz);

	/* tz_diag_pa sits inside TZ XPU-protected OCIMEM; log only the address from HLOS. */
	printf("annibale-modem: tzlog imem tz_diag_pa=%#x hyp_pa=%#x hyp_sz=%#x\n",
	    tz_diag_pa, hyp_pa, hyp_sz);

	if (hyp_pa >= 0x80000000u && hyp_sz >= 0x200u && hyp_sz <= 0x40000u) {
		uint64_t hp_pa = (uint64_t)hyp_pa & ~(uint64_t)PAGE_MASK;
		uint64_t hp_off = (uint64_t)hyp_pa & (uint64_t)PAGE_MASK;
		vm_size_t hp_sz = (vm_size_t)((hp_off + hyp_sz + PAGE_MASK) & ~(uint64_t)PAGE_MASK);
		vm_offset_t h_map = ml_io_map_unmappable((vm_offset_t)hp_pa, hp_sz, VM_WIMG_WCOMB);
		if (h_map != 0) {
			vm_offset_t h = h_map + (vm_offset_t)hp_off;
			__asm__ volatile("dmb ishld" ::: "memory");
			uint32_t h_ring_off = *(volatile const uint32_t *)(h + 0x08u);
			uint16_t h_wrap = *(volatile const uint16_t *)(h + 0x0cu);
			uint16_t h_pos = *(volatile const uint16_t *)(h + 0x0eu);
			uint32_t h_len = *(volatile const uint32_t *)(h + 0x10u);
			if (h_ring_off < 0x20u || h_ring_off >= hyp_sz) {
				h_ring_off = (h_len > 0 && h_len < hyp_sz) ? (hyp_sz - h_len) : 0x200u;
			}
			printf("annibale-modem: hyplog off=%#x wrap=%u pos=%u len=%#x\n",
			    h_ring_off, h_wrap, h_pos, h_len);

			volatile const uint8_t *h_ring = (volatile const uint8_t *)(h + h_ring_off);
			uint32_t h_cap = hyp_sz - h_ring_off;
			if (h_len >= 64u && h_len <= h_cap && (h_wrap != 0 || h_pos > 0)) {
				modem_log_ring_tail("hyplog", h_ring, h_len, h_pos % h_len,
				    h_wrap != 0 || h_pos >= h_len, false);
			} else {
				uint32_t end = h_cap;
				while (end > 0 && (h_ring[end - 1u] < 0x20 || h_ring[end - 1u] >= 0x7f)) {
					end--;
				}
				if (end > 0) {
					uint32_t start = end > 1024u ? end - 1024u : 0u;
					char raw[1024];
					uint32_t take = end - start;
					for (uint32_t i = 0; i < take; i++) {
						raw[i] = (char)h_ring[start + i];
					}
					modem_print_sanitized_chunks("hyplog", raw, take, false);
				}
			}
			ml_io_unmap(h_map, hp_sz);
		}
	}
}

static void
modem_log_crash_reason(void)
{
	uint32_t sz = 0, at = smem_find_mpss_item(ITEM_MPSS_CRASH_REASON, &sz);
	char raw[256] = { 0 }, clean[256] = { 0 };
	uint32_t len;

	if (at == 0 || sz == 0) {
		at = smem_find_item_in_partition(SMEM_HOST_GLOBAL, SMEM_HOST_GLOBAL, ITEM_MPSS_CRASH_REASON, &sz);
	}
	if (at == 0 || sz == 0) {
		return;
	}
	len = sz < sizeof(raw) - 1 ? sz : (uint32_t)(sizeof(raw) - 1);
	smem_read_bytes(at, raw, len);
	modem_sanitize_crash_str(raw, len, clean, sizeof(clean));
	if (clean[0] != '\0') {
		printf("annibale-modem: MPSS crash reason (SMEM 421): %s\n", clean);
	}
}

static void
mpss_glink_log_stall_diag(unsigned tick)
{
	uint32_t d[8] = { 0 }, oq[4] = { 0 }, tq[4] = { 0 };
	uint32_t s_val = 0, s_flags = 0, part_sz = 0, part_cl = 0;
	uint32_t part = smem_find_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, &part_sz, &part_cl);
	bool has_smp2p = smp2p_mpss_read_slave(&s_val, &s_flags);
	uint32_t ipcc_id = 0, ipcc_ver = 0, ipcc_clr = 0;

	if (mpss_glink.desc_off != 0 && (uint64_t)mpss_glink.desc_off + 32u <= SM8750_SMEM_SIZE) {
		for (unsigned i = 0; i < 8u; i++) {
			d[i] = smem_rd(mpss_glink.desc_off + i * 4u);
		}
	}
	if (mpss_glink.our_q_off != 0 && (uint64_t)mpss_glink.our_q_off + 16u <= SM8750_SMEM_SIZE &&
	    mpss_glink.their_q_off != 0 && (uint64_t)mpss_glink.their_q_off + 16u <= SM8750_SMEM_SIZE) {
		for (unsigned i = 0; i < 4u; i++) {
			oq[i] = smem_rd(mpss_glink.our_q_off + i * 4u);
			tq[i] = smem_rd(mpss_glink.their_q_off + i * 4u);
		}
	}
	if (ipcc_base != 0) {
		ipcc_id = *(volatile uint32_t *)(ipcc_base + 0x00u);
		ipcc_ver = *(volatile uint32_t *)(ipcc_base + 0x04u);
		ipcc_clr = *(volatile uint32_t *)(ipcc_base + 0x08u);
		__builtin_arm_dsb(0xf);
	}
	printf("annibale-modem: GLINK diag tick=%u agreed=%d asked=%d ack=%d rcid=%u tx=%u rx=%u "
	    "ptrs=%u/%u/%u/%u d4..7=%#x/%#x/%#x/%#x\n",
	    tick, mpss_glink.agreed ? 1 : 0, mpss_glink.ipcrtr_asked ? 1 : 0,
	    mpss_glink.ipcrtr_ack ? 1 : 0, mpss_glink.their_cid,
	    mpss_glink.tx_count, mpss_glink.rx_count, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
	printf("annibale-modem: GLINK diag2 our=[%#x,%#x,%#x,%#x] their=[%#x,%#x,%#x,%#x] "
	    "ipcc=(%d,%#x,%#x,%#x) smp2p=(%d,%#x,%#x) part=(%#x,%#x,%u)\n",
	    oq[0], oq[1], oq[2], oq[3], tq[0], tq[1], tq[2], tq[3],
	    ipcc_base != 0 ? 1 : 0, ipcc_id, ipcc_ver, ipcc_clr,
	    has_smp2p ? 1 : 0, s_val, s_flags, part, part_sz, part_cl);
	modem_log_crash_reason();
}

static void
modem_bringup_sequence(void)
{
	extern bool adreno8_init_done;
	extern bool annibale_adsp_is_hw_up(void);
	uint32_t dtb_pas = MPSS_DTB_PAS_ID, fw_pas = MPSS_FW_PAS_ID;
	uint32_t part_sz = 0, part_cacheline = 0, part_off;

	part_off = smem_find_partition(SMEM_HOST_APPS, SMEM_HOST_MPSS, &part_sz, &part_cacheline);
	if (modem_boot_arg == 1) {
		bool has_dtb = annibale_fw_exists("modem_dtb.mdt");
		bool has_ipa = annibale_fw_exists("ipa_fws.mdt");
		bool has_fw = annibale_fw_exists("modem.mdt");

		printf("annibale-modem: modem=1 survey (SMEM=0x%llx, part=(off=%#x, sz=%#x, cl=%u), "
		    "fw=(dtb=%d, ipa=%d, modem=%d))\n",
		    smem_phys_active, part_off, part_sz, part_cacheline, has_dtb, has_ipa, has_fw);
		return;
	}

	for (unsigned wait = 0; wait < 120u && !adreno8_init_done; wait++) {
		modem_sleep_ms(100);
	}
	for (unsigned wait = 0; wait < 50u && !annibale_fw_exists("modem.mdt"); wait++) {
		modem_sleep_ms(100);
	}
	if (!annibale_fw_exists("modem.mdt")) {
		printf("annibale-modem: modem.mdt not found on modemfirmware/modem partitions or /fw\n");
		return;
	}
	/* Wait for LPASS ADSP TrustZone PAS calls to finish so modem PAS calls do not contend. */
	for (unsigned wait = 0; wait < 80u && !annibale_adsp_is_hw_up(); wait++) {
		modem_sleep_ms(100);
	}

	lck_mtx_lock(&modem_lock);
	modem_status.state = MODEM_STATE_BOOTING;
	modem_sync_sysctls();
	lck_mtx_unlock(&modem_lock);

	/* ipa_fws belongs to ipa_hw@3e00000 (ipa_gsi_mem 0x9b090000) after IPA clocks are up, not modem_pas. */
	mpss_pas_dtb_authed = false;
	mpss_pas_fw_authed = false;
	mpss_stopping = false;
	if (modem_boot_arg == 2) {
		if (annibale_load_mdt_image("modem_dtb", MPSS_DTB_PAS_ID, MPSS_DTB_PAS_ID_ALT,
		    MPSS_DTB_MEM_PHYS, MPSS_DTB_MEM_SIZE, &dtb_pas)) {
			int64_t rc = annibale_tz_pas_auth_reset(dtb_pas);
			if (rc == 0) {
				mpss_pas_dtb_authed = true;
			}
			printf("annibale-modem: modem=2 PAS_AUTH_AND_RESET(modem_dtb, pas=%u) -> %lld\n", dtb_pas, rc);
		}
		return;
	}

	bool cx_ok = annibale_rpmh_arc_vote("cx.lvl", ANNIBALE_RPMPD_LEVEL_TURBO);
	bool mss_ok = annibale_rpmh_arc_vote("mss.lvl", ANNIBALE_RPMPD_LEVEL_NOM_L1);
	bool mx_ok = annibale_rpmh_arc_vote("mx.lvl", ANNIBALE_RPMPD_LEVEL_TURBO);
	bool aoss_ok = annibale_aoss_send_msg("{class: image, res: load_state, name: modem, val: on}");
	bool ce0_ok = annibale_rpmh_bcm_vote("CE0", 1600ULL * 1000000ULL, 0);
	bool cn0_ok = annibale_rpmh_bcm_vote("CN0", 300ULL * 1000000ULL, 0);
	bool sh0_ok = annibale_rpmh_bcm_vote("SH0", 1600ULL * 1000000ULL, 0);
	bool mc0_ok = annibale_rpmh_bcm_vote("MC0", 1600ULL * 1000000ULL, 0);
	printf("annibale-modem: rails cx=%d mss=%d mx=%d aoss=%d ce0=%d cn0=%d sh0=%d mc0=%d\n",
	    cx_ok ? 1 : 0, mss_ok ? 1 : 0, mx_ok ? 1 : 0, aoss_ok ? 1 : 0,
	    ce0_ok ? 1 : 0, cn0_ok ? 1 : 0, sh0_ok ? 1 : 0, mc0_ok ? 1 : 0);

	uint64_t sup1 = 0, sup_dtb = 0, sup_fw = 0;
	int64_t rc_sup1 = annibale_tz_pas_is_supported(1u, &sup1);
	int64_t rc_sup_dtb = annibale_tz_pas_is_supported(dtb_pas, &sup_dtb);
	int64_t rc_sup_fw = annibale_tz_pas_is_supported(fw_pas, &sup_fw);
	printf("annibale-modem: PAS_IS_SUPPORTED pas1=(%lld,%#llx) pas%u=(%lld,%#llx) pas%u=(%lld,%#llx)\n",
	    rc_sup1, sup1, dtb_pas, rc_sup_dtb, sup_dtb, fw_pas, rc_sup_fw, sup_fw);

	smp2p_mpss_init_ours();
	mpss_glink_init_preboot();

	uint64_t dsm1_res1 = 0, dsm2_res1 = 0;
	/* sun_mpss_resource sets region_assign_shared=0: exclusive VMID_MSS_MSA ownership for DSM 1 & 2. */
	int64_t dsm1_rc = annibale_tz_assign_mem(MPSS_DSM1_MEM_PHYS, MPSS_DSM1_MEM_SIZE,
	    MPSS_VMID_MSA, false, &dsm1_res1);
	int64_t dsm2_rc = annibale_tz_assign_mem(MPSS_DSM2_MEM_PHYS, MPSS_DSM2_MEM_SIZE,
	    MPSS_VMID_MSA, false, &dsm2_res1);
	printf("annibale-modem: HYP_ASSIGN(excl) dsm1=(%lld,%#llx) dsm2=(%lld,%#llx)\n",
	    dsm1_rc, dsm1_res1, dsm2_rc, dsm2_res1);
	if (dsm1_rc != 0 || dsm1_res1 != 0 || dsm2_rc != 0 || dsm2_res1 != 0) {
		modem_log_tz_diag();
		return;
	}

	if (!annibale_load_mdt_image("modem_dtb", MPSS_DTB_PAS_ID, MPSS_DTB_PAS_ID_ALT,
	    MPSS_DTB_MEM_PHYS, MPSS_DTB_MEM_SIZE, &dtb_pas)) {
		printf("annibale-modem: stage failed (dtb_ok=0)\n");
		return;
	}
	uint64_t dtb_res1 = 0;
	int64_t dtb_rc = annibale_tz_pas_auth_reset_detail(dtb_pas, &dtb_res1);
	if (dtb_rc == 0 && dtb_res1 == 0) {
		mpss_pas_dtb_authed = true;
	}
	printf("annibale-modem: PAS_AUTH_AND_RESET(modem_dtb, pas=%u) -> rc=%lld (%#llx) res1=%#llx\n",
	    dtb_pas, dtb_rc, (uint64_t)dtb_rc, dtb_res1);

	if (!annibale_load_mdt_image("modem", MPSS_FW_PAS_ID, 0,
	    MPSS_FW_MEM_PHYS, MPSS_FW_MEM_SIZE, &fw_pas)) {
		printf("annibale-modem: stage failed (dtb_ok=1 fw_ok=0)\n");
		return;
	}
	uint64_t fw_res1 = 0;
	int64_t fw_rc = annibale_tz_pas_auth_reset_detail(fw_pas, &fw_res1);
	if (fw_rc == 0 && fw_res1 == 0) {
		mpss_pas_fw_authed = true;
	}
	printf("annibale-modem: PAS_AUTH_AND_RESET(modem, pas=%u) -> rc=%lld (%#llx) res1=%#llx\n",
	    fw_pas, fw_rc, (uint64_t)fw_rc, fw_res1);

	bool mpss_ready = false, mpss_fatal = false;
	if (fw_rc == 0 && fw_res1 == 0) {
		bool saw_slave = false;
		uint32_t last_val = 0, last_flags = 0;

		for (unsigned ms = 0; ms < 8000u && !mpss_stopping; ms += 50u) {
			uint32_t val = 0, flags = 0;

			if (smp2p_mpss_read_slave(&val, &flags)) {
				if (!saw_slave || val != last_val || flags != last_flags) {
					printf("annibale-modem: SMP2P slave-kernel val=%#x flags=%#x at %u ms\n",
					    val, flags, ms);
					saw_slave = true;
					last_val = val;
					last_flags = flags;
				}
				if ((val & SMP2P_MPSS_FATAL) != 0) {
					mpss_fatal = true;
					modem_log_crash_reason();
					break;
				}
				if ((val & SMP2P_MPSS_READY) != 0) {
					mpss_ready = true;
					printf("annibale-modem: MPSS SMP2P slave-kernel READY in %u ms\n", ms);
					break;
				}
			}
			modem_sleep_ms(50);
		}
		if (!mpss_ready && !mpss_fatal && !mpss_stopping) {
			printf("annibale-modem: MPSS SMP2P READY timeout after 8000 ms (polling continues)\n");
		}
	}
	if (dtb_rc != 0 || dtb_res1 != 0 || fw_rc != 0 || fw_res1 != 0) {
		modem_log_tz_diag();
	}
	if ((dtb_rc != 0 || dtb_res1 != 0 || fw_rc != 0 || fw_res1 != 0 || (!mpss_ready && !mpss_stopping)) &&
	    !mpss_fatal) {
		modem_log_crash_reason();
	}
}

static void mpss_glink_poll_rx(void);

static void
modem_worker_glink_tick(unsigned tick, bool *smp2p_seen, uint32_t *smp2p_val, uint32_t *smp2p_flags,
    bool *newly_open_out, bool *retry_hello_out, bool *is_open_out,
    uint32_t *rcid_out, uint32_t *intent_out)
{
	bool run_diag = false;

	*newly_open_out = false;
	*retry_hello_out = false;
	*is_open_out = false;
	if (smem_base == NULL || mpss_stopping) {
		return;
	}
	if (tick % 4u == 0) {
		uint32_t sv = 0, sf = 0;
		if (smp2p_mpss_read_slave(&sv, &sf) &&
		    (!*smp2p_seen || sv != *smp2p_val || sf != *smp2p_flags)) {
			if (*smp2p_seen || (sv & SMP2P_MPSS_FATAL) != 0 || (sv & SMP2P_MPSS_READY) != 0) {
				printf("annibale-modem: SMP2P slave-kernel runtime val=%#x flags=%#x tick=%u\n",
				    sv, sf, tick);
				if ((sv & SMP2P_MPSS_FATAL) != 0) {
					modem_log_crash_reason();
				}
			}
			*smp2p_seen = true;
			*smp2p_val = sv;
			*smp2p_flags = sf;
		}
	}
	if (!mpss_glink.ready) {
		(void)mpss_glink_locate_queues();
	}
	mpss_glink_poll_rx();
	lck_mtx_lock(&mpss_glink_lock);
	if (mpss_glink.ready && !mpss_glink.ipcrtr_open && tick >= 10u && !mpss_glink.logged_stall) {
		mpss_glink.logged_stall = true;
		run_diag = true;
		if (!mpss_glink.version_sent) {
			mpss_glink.version_sent = mpss_glink_write_raw(GLINK_CMD_VERSION, 1u,
			    mpss_glink.our_features, NULL, 0);
		} else {
			ipcc_ring_mpss(IPCC_SIG_GLINK);
		}
	}
	if (mpss_glink.ipcrtr_open && !mpss_glink.qrtr_attached) {
		mpss_glink.qrtr_attached = true;
		mpss_glink.qrtr_hello_pending = !mpss_glink_has_free_intent(64u);
		*newly_open_out = true;
		*rcid_out = mpss_glink.their_cid;
		*intent_out = mpss_glink.their_intent_count;
	} else if (mpss_glink.qrtr_attached && mpss_glink.qrtr_hello_pending &&
	    mpss_glink_has_free_intent(64u)) {
		mpss_glink.qrtr_hello_pending = false;
		*retry_hello_out = true;
	}
	*is_open_out = mpss_glink.ipcrtr_open;
	lck_mtx_unlock(&mpss_glink_lock);
	if (run_diag) {
		mpss_glink_log_stall_diag(tick);
	}
}

static bool
modem_pas_shutdown_active(bool send_aoss_off)
{
	bool ok = true;

	if (!mpss_pas_fw_authed && !mpss_pas_dtb_authed) {
		return true;
	}
	smp2p_mpss_set_stop();
	if (mpss_pas_fw_authed) {
		uint64_t res1 = 0;
		int64_t rc = annibale_tz_pas_shutdown(MPSS_FW_PAS_ID, &res1);
		printf("annibale-modem: PAS_SHUTDOWN(modem, pas=%u) -> rc=%lld res1=%#llx\n",
		    MPSS_FW_PAS_ID, rc, res1);
		if (rc == 0 && res1 == 0) {
			mpss_pas_fw_authed = false;
		} else {
			ok = false;
		}
	}
	if (mpss_pas_dtb_authed) {
		uint64_t res1 = 0;
		int64_t rc = annibale_tz_pas_shutdown(MPSS_DTB_PAS_ID, &res1);
		printf("annibale-modem: PAS_SHUTDOWN(modem_dtb, pas=%u) -> rc=%lld res1=%#llx\n",
		    MPSS_DTB_PAS_ID, rc, res1);
		if (rc == 0 && res1 == 0) {
			mpss_pas_dtb_authed = false;
		} else {
			ok = false;
		}
	}
	if (send_aoss_off) {
		(void)annibale_aoss_send_msg("{class: image, res: load_state, name: modem, val: off}");
	}
	return ok;
}

void annibale_modem_stop(void);
bool annibale_modem_return_quiesce(void);

void
annibale_modem_stop(void)
{
	if (modem_boot_arg == 0 || mpss_stopping) {
		return;
	}
	mpss_stopping = true;
	if (!mpss_pas_fw_authed && !mpss_pas_dtb_authed) {
		return;
	}
	smp2p_mpss_set_stop();
	modem_sleep_ms(50);
	(void)modem_pas_shutdown_active(true);
}

bool
annibale_modem_return_quiesce(void)
{
	mpss_stopping = true;
	if (modem_boot_arg == 0 || (!mpss_pas_fw_authed && !mpss_pas_dtb_authed)) {
		return true;
	}
	return modem_pas_shutdown_active(false);
}

#endif /* _ANNIBALE_MODEM_BOOT_INC_ */

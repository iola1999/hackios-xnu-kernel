/*
 * APFS Library Write Helpers
 */
#pragma once

static bool names_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool fold);

int
apfs_omap_lookup(struct apfs_omap *omap, uint64_t oid, apfs_xid_t want_xid, uint64_t *addr)
{
	return omap_find(omap, oid, want_xid, addr);
}

int
apfs_xfields_find(const uint8_t *buf, uint16_t len, uint8_t want, const uint8_t **out,
    uint16_t *out_len)
{
	uint16_t num, i, off;

	*out = NULL;
	*out_len = 0;
	if (len < 4) {
		return ENOENT;
	}
	num = le16(buf);
	if ((uint32_t)4 + 4 * (uint32_t)num > len) {
		return EIO;
	}
	off = (uint16_t)(4 + 4 * num);
	for (i = 0; i < num; i++) {
		uint8_t xtype = buf[4 + 4 * i];
		uint16_t xsize = le16(buf + 4 + 4 * i + 2);
		uint32_t pad = ((uint32_t)xsize + 7u) & ~7u;

		if ((uint32_t)off + xsize > len) {
			return EIO;
		}
		if (xtype == want) {
			*out = buf + off;
			*out_len = xsize;
			return 0;
		}
		if ((uint32_t)off + pad > len) {
			return ENOENT;  /* the last field, padding cut short */
		}
		off = (uint16_t)(off + pad);
	}
	return ENOENT;
}

/* Writers drop what a transaction overwrote: a cached block, an object map hit, every hit. */
void
apfs_nx_cache_forget(struct apfs_nx *nx, uint64_t addr)
{
	int32_t *pp;

	for (pp = &nx->cache_hash[cache_bucket(addr)]; *pp >= 0; pp = &nx->cache[*pp].next) {
		struct apfs_buf *b = &nx->cache[*pp];

		if (b->addr == addr) {
			*pp = b->next;
			if (nx->unlock != NULL && b->data != NULL) {
				cache_retire(nx, b->data);
				b->data = NULL;
			}
			b->addr = APFS_BUF_EMPTY;
			b->next = -1;
			b->ref = false;
			return;
		}
	}
}

void
apfs_omap_cache_forget(struct apfs_omap *omap, uint64_t oid)
{
	struct apfs_nx *nx = omap->tree.nx;
	uint64_t h = (oid ^ (omap->tree.root * 0xff51afd7ed558ccdULL)) * 0x9E3779B97F4A7C15ULL;
	struct apfs_omap_hit *hit;

	if (nx == NULL || nx->omap_cache == NULL) {
		return;
	}
	hit = &nx->omap_cache[h >> 53 & (APFS_OMAP_CACHE - 1)];
	if (hit->oid == oid) {
		memset(hit, 0, sizeof(*hit));
	}
}

void
apfs_omap_cache_clear(struct apfs_nx *nx)
{
	if (nx->omap_cache != NULL) {
		memset(nx->omap_cache, 0, APFS_OMAP_CACHE * sizeof(*nx->omap_cache));
	}
}

int
apfs_meta_block(struct apfs_nx *nx, uint64_t addr, const uint8_t **out)
{
	return nx_block(nx, addr, out, true);
}

bool
apfs_names_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool fold)
{
	return names_equal(a, a_len, b, b_len, fold);
}

#ifdef KERNEL
#include <sys/time.h>

uint64_t
apfs_now_ns(void)
{
	struct timespec ts;

	nanotime(&ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/*
 * APFS Library Write Helpers
 */
#pragma once

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

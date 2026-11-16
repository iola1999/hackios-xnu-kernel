/*
 * A volume built over data that is already on the disk, for the host tool only (apfs-tool
 * annfs, which turns the ANNFS001 data volume into APFS where it lies).  Included by
 * apfs_write.c outside the kernel; the API is at the end of apfs_write.h.
 *
 * The data is not copied.  apfs_w_adopt() maps a file's bytes to the blocks they are in,
 * through the same path that gives a hole new blocks (apfs_w_map), with the allocator told
 * which blocks to take (w->place).  Every other allocation stays below w->alloc_end, so the
 * metadata written on the way never lands on data that has not been adopted yet.
 */
#pragma once

void
apfs_w_ids_from(struct apfs_wtx *w, uint64_t next)
{
	if (w->vol->next_obj_id < next) {
		w->vol->next_obj_id = next;
	}
}

void
apfs_w_alloc_below(struct apfs_wtx *w, uint64_t end)
{
	w->alloc_end = end;
}

int
apfs_w_adopt(struct apfs_wtx *w, uint64_t id, uint64_t off, uint64_t phys, uint64_t blocks)
{
	int error = off % w->bs != 0 || phys == 0 || phys + blocks > w->sp.block_count ? EINVAL : 0;

	while (error == 0 && blocks > 0) {
		uint64_t p = 0, run = 0, n;

		w->place = phys;
		error = apfs_w_map(w, id, off, blocks * w->bs, true, &p, &run);
		w->place = 0;
		if (error == 0 && p != phys) {
			error = p == 0 ? EINVAL : EEXIST;       /* past the end, or data there already */
		}
		if (error == 0) {
			n = run / w->bs < blocks ? run / w->bs : blocks;
			off += n * w->bs;
			phys += n;
			blocks -= n;
		}
	}
	return error;
}

int
apfs_w_set_docid(struct apfs_wtx *w, uint64_t id, uint32_t docid)
{
	struct winode *i = apfs_alloc(sizeof(*i));
	int error = i == NULL ? ENOMEM : wop_begin(w, 0);
	uint8_t d[4];

	if (error == 0) {
		error = wi_load(w, id, i);
	}
	if (error == 0) {
		w_put_le32(d, docid);
		error = wi_xf_set(i, APFS_INO_EXT_TYPE_DOCUMENT_ID, 0, d, 4);
		if (error == 0) {
			error = wi_store(w, i);
		}
		if (error == 0 && docid >= w_le32(w->vsb + VSB_NEXT_DOC_ID)) {
			w_put_le32(w->vsb + VSB_NEXT_DOC_ID, docid + 1);
		}
		error = wop_end(w, error);
	}
	if (i != NULL) {
		apfs_free(i, sizeof(*i));
	}
	return error;
}

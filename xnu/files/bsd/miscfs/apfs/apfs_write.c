/*
 * Writing into APFS.  Two writers live here.
 *
 * The transaction engine (apfs_wbt.inc, apfs_wspace.inc, apfs_wtx.inc, apfs_wfs.inc,
 * apfs_wops.inc, apfs_wxattr.inc, apfs_wclone.inc; the model is in apfs_wtx.h) is what a
 * writable mount uses: copy-on-write B-trees, the space manager, the object maps and the
 * checkpoint rings, with one open transaction made durable by apfs_wtx_commit().  The kernel's
 * vnode layer (apfs_vnops_write.inc) and the host tool (tools/apfs, `apfs-tool do`) call it
 * through apfs_write.h.
 *
 * apfs_write_in_place() below is older and much narrower: it replaces bytes inside a file that
 * already exists without changing its length, its extents or any other structure, so no
 * transaction is involved.  A power failure can leave a mixture of the old and new contents of
 * that file and nothing else; every tree and checksum stays as it was.  Used by apfs_check=7
 * (bsd/dev/annibale/annibale_apfs_check.c) and `apfs-tool write`.
 *
 * What the in-place writer refuses.  Every one of these is read off the volume before a byte
 * goes out:
 *
 *   the device cannot write                 dev.write is NULL
 *   a sealed volume                         its integrity metadata holds a hash of each file's
 *                                           contents, and we do not update them
 *   a volume with snapshots                 a snapshot refers to these very blocks
 *   no extent-reference tree                then nothing can say the blocks are not shared
 *   not a regular file, or no data stream
 *   a compressed file                       its bytes are in an attribute or a resource fork,
 *                                           and its decmpfs header states the size
 *   past the end of the file                a write that grows a file changes its records
 *   a hole in the range                     filling one means allocating
 *   an extent whose extent-reference record does not begin at the same block, does not cover
 *       the whole extent, is not owned by this stream, or has more than one reference
 *                                           -- a clone, a snapshot or a tree we misread
 *   a block outside the container, or inside block zero, the checkpoint rings or the container's
 *       object map                          -- the bound that holds even if a tree was misread
 *
 * The bounds are three deep, after the fashion of the writes into the phone's flash
 * (bsd/dev/annibale_ufs.c): the file's own extents say which blocks are its contents, the
 * extent-reference tree says the same blocks are that stream's and nobody else's, and a plain
 * range check refuses anything near the container's own structures whatever the trees said.
 * Then every block is read back and compared before the next one goes out.  The file's
 * modification time is not touched, and the caller invalidates any cached pages of the file.
 */

#include "apfs_write.h"
#include "apfs_wtx.h"
#ifdef KERNEL
#include <kern/qsort.h>
#endif

/* ------------------------------------------------------------------ little-endian helpers */

static inline uint16_t
w_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t
w_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
w_le64(const uint8_t *p)
{
	return (uint64_t)w_le32(p) | (uint64_t)w_le32(p + 4) << 32;
}

static inline void
w_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
w_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
w_put_le64(uint8_t *p, uint64_t v)
{
	w_put_le32(p, (uint32_t)v);
	w_put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline void
w_make_jkey(uint8_t *dst, uint64_t oid, uint8_t type)
{
	uint64_t val = (oid & APFS_OBJ_ID_MASK) | ((uint64_t)type << APFS_OBJ_TYPE_SHIFT);
	w_put_le64(dst, val);
}

/* ------------------------------------------------------------------ the volume */

int
apfs_write_volume_ok(struct apfs_vol *vol, const char **why)
{
	const char *reason = NULL;

	if (vol == NULL || vol->nx == NULL) {
		reason = "invalid volume";
	} else if (vol->nx->dev.write == NULL) {
		reason = "the device does not take writes";
	} else if (vol->nx->block_size < APFS_NX_MINIMUM_BLOCK_SIZE) {
		reason = "the container's block size makes no sense";
	} else if (vol->incompat & APFS_INCOMPAT_SEALED_VOLUME) {
		reason = "a sealed volume: its integrity metadata hashes the contents";
	} else if (vol->integrity_meta_oid != 0) {
		reason = "the volume has integrity metadata: the contents are hashed";
	} else if (vol->num_snapshots != 0) {
		reason = "the volume has snapshots, which share its blocks";
	} else if (vol->role == APFS_VOL_ROLE_SYSTEM || vol->role == APFS_VOL_ROLE_PREBOOT) {
		/*
		 * Not an APFS rule but this port's: the system volume of iOS is not ours to
		 * change.  It is worth saying out loud because the gates above do not catch it --
		 * on an iPhone that volume is sealed, but the copy this port boots from was
		 * unpacked out of an IPSW and is not, so it would otherwise pass every check
		 * here.  What keeps it safe on the phone is where a write may go at all (the
		 * loader's BLOCK_WRITE_FIRST and a kernel whose block devices refuse every
		 * write); this is one more fence in front of that, not instead of it.
		 */
		reason = "the volume is iOS's own system or preboot volume";
	} else if (vol->extentref_oid == 0) {
		reason = "no extent-reference tree: nothing can say a block is not shared";
	}
	if (why != NULL) {
		*why = reason;
	}
	return reason == NULL ? 0 : EROFS;
}

/*
 * Blocks [start, start + count) may hold the contents of a file and nothing else.  This knows
 * nothing of any tree: it is the bound that stands even if every tree was read wrongly.
 */
static bool
blocks_are_free_ground(struct apfs_nx *nx, uint64_t start, uint64_t count)
{
	uint64_t end;

	if (nx == NULL || count == 0 || start == 0 || start >= nx->block_count) {
		return false;
	}
	if (count > nx->block_count - start) {
		return false;
	}
	end = start + count;
	/* the two checkpoint rings, as the live container superblock names them */
	if (nx->xp_desc_blocks != 0 && start < nx->xp_desc_base + nx->xp_desc_blocks &&
	    end > nx->xp_desc_base) {
		return false;
	}
	if (nx->xp_data_blocks != 0 && start < nx->xp_data_base + nx->xp_data_blocks &&
	    end > nx->xp_data_base) {
		return false;
	}
	/* the container's object map, which is at a block address and not behind one */
	if (nx->omap_addr >= start && nx->omap_addr < end) {
		return false;
	}
	return true;
}

/*
 * The extent-reference tree's word on an extent: one record, beginning at this very block,
 * covering the whole of it, owned by this stream, referred to once.  Anything else -- a record
 * that begins elsewhere, a shorter one, another owner, a second reference -- is a clone, a
 * snapshot, or a tree we did not read the way the writer of it meant, and is refused.
 */
static int
extent_is_ours(struct apfs_btree *er, uint64_t stream_id, uint64_t phys, uint64_t blocks,
    const char **why)
{
	if (er == NULL || why == NULL) {
		return EINVAL;
	}
	struct apfs_cursor cur;
	struct apfs_rec rec;
	uint64_t len, owner;
	int32_t refcnt;
	int error;

	error = apfs_seek(er, phys, APFS_TYPE_EXTENT, &cur);
	if (error) {
		*why = "the extent-reference tree cannot be read";
		return error;
	}
	error = apfs_cursor_next(&cur, &rec);
	if (error != 0 || rec.obj_id != phys || rec.type != APFS_TYPE_EXTENT) {
		*why = "no extent-reference record begins at this extent's first block";
		return EROFS;
	}
	if (rec.val == NULL || rec.val_len < 20) {
		*why = "an extent-reference record too short to be one";
		return EIO;
	}
	len = w_le64(rec.val) & APFS_PEXT_LEN_MASK;
	owner = w_le64(rec.val + 8);
	refcnt = (int32_t)w_le32(rec.val + 16);
	if (len < blocks) {
		*why = "the extent-reference record covers less than the extent";
		return EROFS;
	}
	if (owner != stream_id) {
		*why = "the extent-reference record names another stream as the owner";
		return EROFS;
	}
	if (refcnt != 1) {
		*why = "the extent is referred to more than once: a clone or a snapshot";
		return EROFS;
	}
	return 0;
}

/* ------------------------------------------------------------------ the write */

/*
 * One block out and read back again.  A device that says a write went through and then hands
 * back something else is the one failure no amount of checking above can catch, so every block
 * is compared before the next goes out -- the same bargain the writes into the phone's flash
 * make (bsd/dev/annibale_ufs.c).
 */
static int
block_out(struct apfs_nx *nx, uint64_t block, const uint8_t *data, uint8_t *scratch)
{
	if (nx == NULL || data == NULL || scratch == NULL) {
		return EINVAL;
	}
	uint64_t at = block * (uint64_t)nx->block_size;
	int error;

	error = nx->dev.write(nx->dev.ctx, at, data, nx->block_size);
	if (error) {
		return error;
	}
	error = nx->dev.read_data != NULL ?
	    nx->dev.read_data(nx->dev.ctx, at, scratch, nx->block_size) :
	    nx->dev.read(nx->dev.ctx, at, scratch, nx->block_size);
	if (error) {
		return error;
	}
	if (memcmp(scratch, data, nx->block_size) != 0) {
		apfs_log("block %llu read back as something else than it was written\n",
		    (unsigned long long)block);
		return EIO;
	}
	return 0;
}

int
apfs_write_in_place(struct apfs_vol *vol, uint64_t obj_id, uint64_t off, const void *buf,
    size_t len, struct apfs_write_report *rep)
{
	struct apfs_write_report unused_report;

	if (rep == NULL) rep = &unused_report;
	memset(rep, 0, sizeof(*rep));
	rep->bytes = len;

	if (vol == NULL || vol->nx == NULL) {
		return EINVAL;
	}
	if (buf == NULL && len > 0) {
		return EINVAL;
	}
	if (off + len < off) {
		return EINVAL;
	}
	struct apfs_nx *nx = vol->nx;
	struct apfs_inode ino;
	struct apfs_xattr xa;
	struct apfs_btree er;
	const uint8_t *src = buf;
	uint8_t *block = NULL, *scratch = NULL;
	uint32_t bs = nx->block_size;
	uint64_t at = off, left = len;
	const char *why = NULL;
	int error;

	error = apfs_write_volume_ok(vol, &why);
	if (error) {
		goto refuse;
	}
	error = apfs_inode_get(vol, obj_id, &ino);
	if (error) {
		why = "there is no such inode";
		goto refuse;
	}
	if ((ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		why = "not a regular file";
		error = EROFS;
		goto refuse;
	}
	if (!ino.has_dstream) {
		why = "the file has no data stream";
		error = EROFS;
		goto refuse;
	}
	/*
	 * A compressed file keeps its bytes in an attribute or a resource fork and its decmpfs
	 * header states the uncompressed size, so writing over its extents writes over nothing
	 * a reader looks at -- or over the resource fork, which is a different stream.
	 */
	error = apfs_xattr_lookup(vol, obj_id, APFS_DECMPFS_XATTR_NAME, &xa);
	if (error == 0) {
		why = "the file is compressed: its bytes are not in its extents";
		error = EROFS;
		goto refuse;
	}
	if (error != ENOENT) {
		why = "the file's attributes cannot be read";
		goto refuse;
	}
	if (len == 0) {
		error = 0;
		goto out;
	}
	if (off > ino.size || len > ino.size - off) {
		why = "the write would go past the end of the file";
		error = EROFS;
		goto refuse;
	}
	error = apfs_extentref_open(vol, &er);
	if (error) {
		why = "the extent-reference tree cannot be opened";
		goto refuse;
	}
	block = apfs_alloc(bs);
	scratch = apfs_alloc(bs);
	if (block == NULL || scratch == NULL) {
		why = "no memory";
		error = ENOMEM;
		goto refuse;
	}

	/*
	 * An extent at a time.  Looking one up walks the stream's extents from the front, so a
	 * write across a badly fragmented file costs more than it looks -- which is fine for what
	 * this is for (a file of a few extents written whole) and would not be for a filesystem.
	 */
	while (left > 0) {
		struct apfs_extent ext;
		uint64_t ext_blocks, piece;

		error = apfs_extent_at(vol, ino.private_id, at, &ext);
		if (error) {
			why = "no extent of the file covers this offset";
			error = error == ENOENT ? EROFS : error;
			goto refuse;
		}
		if (ext.phys == 0) {
			why = "a hole in the range: filling one means allocating blocks";
			error = EROFS;
			goto refuse;
		}
		ext_blocks = ext.length / bs;
		if (!blocks_are_free_ground(nx, ext.phys, ext_blocks)) {
			why = "the extent is not where a file's contents can be";
			error = EROFS;
			goto refuse;
		}
		error = extent_is_ours(&er, ino.private_id, ext.phys, ext_blocks, &why);
		if (error) {
			goto refuse;
		}
		/* how much of this extent the write still wants */
		piece = ext.length - (at - ext.logical);
		if (piece > left) {
			piece = left;
		}

		/*
		 * Whole blocks only: the write path on the phone takes nothing else, and a
		 * partial block is the file's own bytes either way, so it is read, patched and
		 * written back whole.
		 */
		while (piece > 0) {
			uint64_t in_block = (at - ext.logical) % bs;
			uint64_t take = bs - in_block;
			uint64_t b = ext.phys + (at - ext.logical) / bs;

			if (take > piece) {
				take = piece;
			}
			if (in_block == 0 && take == bs) {
				memcpy(block, src, bs);
			} else {
				error = nx->dev.read_data != NULL ?
				    nx->dev.read_data(nx->dev.ctx, b * (uint64_t)bs, block, bs) :
				    nx->dev.read(nx->dev.ctx, b * (uint64_t)bs, block, bs);
				if (error) {
					why = "the block to be patched cannot be read";
					goto refuse;
				}
				memcpy(block + in_block, src, (size_t)take);
				rep->partial++;
			}
			error = block_out(nx, b, block, scratch);
			rep->calls++;
			if (error) {
				why = "the device did not take the block";
				goto refuse;
			}
			if (rep->blocks == 0) {
				rep->first_block = b;
			}
			rep->blocks++;
			src += take;
			at += take;
			piece -= take;
			left -= take;
		}
	}
	error = 0;
	goto out;

refuse:
	if (why == NULL) {
		why = "the write failed";
	}
	rep->why = why;
	apfs_log("write of %llu bytes at %llu of inode %llu refused: %s\n",
	    (unsigned long long)len, (unsigned long long)off, (unsigned long long)obj_id, why);
out:
	if (block != NULL) {
		apfs_free(block, bs);
	}
	if (scratch != NULL) {
		apfs_free(scratch, bs);
	}
	return error;
}


#include "apfs_wbt.inc"
#include "apfs_wspace.inc"
#include "apfs_wtx.inc"
#include "apfs_wfs.inc"
#include "apfs_wops.inc"
#include "apfs_wxattr.inc"
#include "apfs_wclone.inc"

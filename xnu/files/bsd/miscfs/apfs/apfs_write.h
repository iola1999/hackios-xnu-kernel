/*
 * Writing into APFS (apfs_write.c): the transaction engine a writable mount uses, and the
 * narrower in-place writer that replaces bytes of an existing file without a transaction.
 */

#ifndef _MISCFS_APFS_APFS_WRITE_H_
#define _MISCFS_APFS_APFS_WRITE_H_

#include "apfs_lib.h"

/* What a write did, or why it was refused.  Filled in whether it went well or not. */
struct apfs_write_report {
	const char *why;                /* NULL when the write went through */
	uint64_t    first_block;        /* the first block written, 0 if none was */
	uint64_t    blocks;             /* blocks written */
	uint64_t    bytes;              /* bytes of the file the caller asked to replace */
	uint64_t    partial;            /* blocks read, patched and written back whole */
	uint64_t    calls;              /* calls to dev.write */
};

/*
 * May this volume be written at all?  Answered from the volume alone, before any file is
 * named: 0, or EROFS with the reason in *why.  apfs_write_in_place() asks this itself; it
 * is here so that a caller can say at mount time whether writing is possible.
 */
int apfs_write_volume_ok(struct apfs_vol *vol, const char **why);

/*
 * Replace `len` bytes at offset `off` of the file `obj_id` with `buf`, leaving its length,
 * its extents and every other structure of the container exactly as they were.  Refuses
 * (EROFS, with the reason in rep->why) anything it cannot do that way -- see apfs_write.c.
 *
 * The blocks go out in ascending order of offset and each is read back and compared before
 * the next, so a caller that puts its own checksum in the last bytes of what it writes can
 * tell afterwards whether a write that was cut short got there.
 */
int apfs_write_in_place(struct apfs_vol *vol, uint64_t obj_id, uint64_t off,
    const void *buf, size_t len, struct apfs_write_report *rep);

/*
 * A writable volume: one open transaction at a time, kept in memory and made durable by
 * apfs_wtx_commit() (apfs_wtx.h has the model).  Every call below needs the caller's
 * exclusive lock round the container.  An operation either completes in the transaction or
 * leaves it untouched; a failure part way through (EIO) discards the transaction and the
 * mount stays read-only (apfs_wtx_broken).
 */
struct apfs_wtx;

int  apfs_wtx_attach(struct apfs_vol *vol, struct apfs_wtx **out);
void apfs_wtx_detach(struct apfs_wtx *w);
int  apfs_wtx_commit(struct apfs_wtx *w);
bool apfs_wtx_dirty(const struct apfs_wtx *w);          /* changes not yet committed */
bool apfs_wtx_broken(const struct apfs_wtx *w);
uint64_t apfs_wtx_ops(const struct apfs_wtx *w);        /* mutating ops begun on this handle */
bool apfs_wtx_discard_solo(struct apfs_wtx *w, uint64_t expected_ops);
uint32_t apfs_wtx_dirty_blocks(const struct apfs_wtx *w);
uint64_t apfs_wtx_free_blocks(const struct apfs_wtx *w);
void apfs_wtx_stats(const struct apfs_wtx *w, uint64_t *commits, uint64_t *blocks, uint64_t *ticks);

/* What a new object is: its mode (with the type bits), owner, device number or link target. */
struct apfs_wnew {
	uint16_t    mode;
	uint32_t    uid, gid;
	uint32_t    rdev;                   /* character and block devices */
	const char *target;                 /* symbolic links: the target, link_len bytes */
	size_t      target_len;
	uint32_t    pclass;                 /* data protection class, 0 for the volume's default */
	uint64_t    id;                     /* 0: the next free number; else one below it not in use */
};

int apfs_w_create(struct apfs_wtx *w, uint64_t dir, const char *name, size_t len,
    const struct apfs_wnew *a, uint64_t *id);
int apfs_w_link(struct apfs_wtx *w, uint64_t id, uint64_t dir, const char *name, size_t len);

/*
 * Take a name away.  want_dir: rmdir (ENOTDIR otherwise, EISDIR the other way round).  If
 * that was the object's last name it goes too (*gone) -- unless keep, when it is still open:
 * then it lives on in the private directory until apfs_w_reap().
 */
int apfs_w_remove(struct apfs_wtx *w, uint64_t dir, const char *name, size_t len, bool want_dir,
    bool keep, uint64_t *id, bool *gone);
int apfs_w_reap(struct apfs_wtx *w, uint64_t id);
int apfs_w_reap_orphans(struct apfs_wtx *w);            /* at mount: what a crash left */

#define APFS_WREN_SWAP 0x1                              /* renamex_np(RENAME_SWAP) */
#define APFS_WREN_EXCL 0x2                              /* renamex_np(RENAME_EXCL) */
int apfs_w_rename(struct apfs_wtx *w, uint64_t fdir, const char *fname, size_t flen,
    uint64_t tdir, const char *tname, size_t tlen, uint32_t flags, bool keep_target,
    uint64_t *replaced, bool *replaced_gone);

#define APFS_WA_MODE   0x001
#define APFS_WA_UID    0x002
#define APFS_WA_GID    0x004
#define APFS_WA_FLAGS  0x008
#define APFS_WA_ATIME  0x010
#define APFS_WA_MTIME  0x020
#define APFS_WA_CTIME  0x040
#define APFS_WA_CRTIME 0x080
#define APFS_WA_PCLASS 0x100
#define APFS_WA_IFLAGS 0x200                    /* internal flags: iflags_on set, iflags_off cleared */
struct apfs_wattr {
	uint32_t set;                       /* APFS_WA_* */
	uint16_t mode;                      /* permission bits only; the type stays */
	uint32_t uid, gid, flags, pclass;
	uint64_t atime, mtime, ctime, crtime;   /* nanoseconds since 1970 */
	uint64_t iflags_on, iflags_off;     /* APFS_INODE_IS_SYNC_ROOT and the like */
};
int apfs_w_setattr(struct apfs_wtx *w, uint64_t id, const struct apfs_wattr *a);

/* A regular file's length: blocks past it freed, or a hole up to it. */
int apfs_w_truncate(struct apfs_wtx *w, uint64_t id, uint64_t size);

/*
 * Where bytes [off, off + len) of a regular file are: *phys the block off lands in (0 for a
 * hole when !alloc), *run how many bytes from off on are contiguous there.  alloc gives
 * blocks to what is a hole within the file's length.
 */
int apfs_w_map(struct apfs_wtx *w, uint64_t id, uint64_t off, uint64_t len, bool alloc,
    uint64_t *phys, uint64_t *run);

/* Write bytes straight to the device, growing the file: the host's stand-in for the page cache. */
int apfs_w_write(struct apfs_wtx *w, uint64_t id, uint64_t off, const void *buf, size_t len);

#define APFS_WX_CREATE  0x1
#define APFS_WX_REPLACE 0x2
int apfs_w_setxattr(struct apfs_wtx *w, uint64_t id, const char *name, const void *data,
    size_t len, uint32_t flags);
int apfs_w_rmxattr(struct apfs_wtx *w, uint64_t id, const char *name);

/*
 * clonefile(2): a copy of the object src (a file, a link or a directory tree) named `name` in
 * dir, with blocks of its own (apfs_wclone.inc).  top's MODE, UID, GID and FLAGS are what the
 * copy at the top gets instead of its source's; with APFS_WCLONE_OWNER every object under it
 * takes top's owner as well.
 */
#define APFS_WCLONE_OWNER 0x1
int apfs_w_clone(struct apfs_wtx *w, uint64_t src, uint64_t dir, const char *name, size_t len,
    const struct apfs_wattr *top, uint32_t flags, uint64_t *id);

/* The object's document id, assigned from the volume's counter the first time. */
int apfs_w_docid(struct apfs_wtx *w, uint64_t id, uint32_t *docid);

#ifndef KERNEL
/*
 * A volume built over data already on the disk (apfs_wadopt.inc; apfs-tool annfs).  Object
 * numbers from `next` on are the volume's to hand out, those below are the caller's to give
 * (apfs_wnew.id).  Nothing is allocated at or past block `end` but what apfs_w_adopt() names:
 * blocks [phys, phys + blocks), free until then, become the file's from byte `off` on, within
 * its length.  apfs_w_set_docid() gives an object the document id it had.
 */
void apfs_w_ids_from(struct apfs_wtx *w, uint64_t next);
void apfs_w_alloc_below(struct apfs_wtx *w, uint64_t end);
int  apfs_w_adopt(struct apfs_wtx *w, uint64_t id, uint64_t off, uint64_t phys, uint64_t blocks);
int  apfs_w_set_docid(struct apfs_wtx *w, uint64_t id, uint32_t docid);
#endif

#endif /* _MISCFS_APFS_APFS_WRITE_H_ */

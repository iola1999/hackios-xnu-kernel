/*
 * A block device over an image holding only some of its blocks: how the real iOS root
 * volume is read on the phone, which cannot hold all of it.  Only with boot-arg
 * apfs_sparse=<offset in md0>.  See bsd/dev/annibale_sparse.c.
 */
#ifndef _SYS_ANNIBALE_SPARSE_H_
#define _SYS_ANNIBALE_SPARSE_H_

void annibale_sparse_init(void);
void annibale_sparse_report(void);

#endif /* _SYS_ANNIBALE_SPARSE_H_ */

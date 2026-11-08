/*
 * A virtio block device for QEMU: a disk image on the host becomes /dev/rvblk0 in the
 * guest.  Only with boot-arg virtio_blk=1.  See bsd/dev/annibale_virtio_blk.c.
 */
#ifndef _SYS_ANNIBALE_VIRTIO_BLK_H_
#define _SYS_ANNIBALE_VIRTIO_BLK_H_

#include <stdbool.h>
#include <sys/types.h>

void annibale_virtio_blk_init(void);
bool annibale_virtio_blk_dev(dev_t dev);    /* one of its devices */

#endif /* _SYS_ANNIBALE_VIRTIO_BLK_H_ */

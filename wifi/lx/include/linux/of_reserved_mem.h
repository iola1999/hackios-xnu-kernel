#ifndef _LX_LINUX_OF_RESERVED_MEM_H
#define _LX_LINUX_OF_RESERVED_MEM_H

#include <linux/of.h>
#include <linux/ioport.h>

/* a region of memory the device tree sets aside (ath12k's AHB chips have their firmware's there) */
struct reserved_mem {
	const char *name;
	phys_addr_t base;
	phys_addr_t size;
};

/* the region a node of the tree describes: there is no tree, and no region */
static inline struct reserved_mem *
of_reserved_mem_lookup(struct device_node *np)
{
	return (NULL);
}

/* the reserved region that entry INDEX of the node's "memory-region" names */
int of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index,
    struct resource *res);

#endif

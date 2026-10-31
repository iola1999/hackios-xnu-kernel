/* The ports and endpoints of the device tree, through which ath12k links several chips: there is one chip here. */
#ifndef _LX_LINUX_OF_GRAPH_H
#define _LX_LINUX_OF_GRAPH_H

#include <linux/of.h>

static inline struct device_node *
of_graph_get_endpoint_by_regs(const struct device_node *parent, int port, int endpoint)
{
	return (NULL);
}

static inline struct device_node *
of_graph_get_remote_endpoint(const struct device_node *node)
{
	return (NULL);
}

static inline struct device_node *
of_graph_get_port_parent(struct device_node *node)
{
	return (NULL);
}

#endif

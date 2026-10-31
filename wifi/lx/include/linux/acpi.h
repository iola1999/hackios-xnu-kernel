/*
 * ACPI, which tells ath12k of a laptop's limits on transmit power and the like. A phone has no ACPI,
 * and FreeBSD's header would bring in all of ACPICA, which has no platform header for an Apple
 * target: this one has nothing, and ath12k is built without CONFIG_ACPI.
 */
#ifndef _LX_LINUX_ACPI_H
#define _LX_LINUX_ACPI_H

#include <linux/device.h>

#endif

/*
 * The kernel's list of what is to be done when it panics. XNU has no such list for the driver to
 * join: ath12k's wish to dump its firmware's state then goes unheard.
 */
#ifndef _LX_LINUX_PANIC_NOTIFIER_H
#define _LX_LINUX_PANIC_NOTIFIER_H

#include <linux/notifier.h>

#define atomic_notifier_chain_register(list, block)	((void)(block), 0)
#define atomic_notifier_chain_unregister(list, block)	((void)(block), 0)

#endif

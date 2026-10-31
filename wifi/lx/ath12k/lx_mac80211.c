/*
 * What ath12k asks of mac80211 beyond what ath11k did, next to src/lx_80211.c: the work that
 * mac80211 runs for a driver under the radio's lock ("wiphy work": ath12k sends its management
 * frames and cleans up after a scan this way), and a question about the queues.
 */
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include <lx_os.h>
#include "lx.h"

/* There is one radio: its work waiting, in order, and the work item that runs it. */
static struct {
	struct list_head waiting;
	struct workqueue_struct *queue;
	struct work_struct runner;
	struct wiphy *wiphy;
	volatile bool ready;
} works;

/* as cfg80211's wiphy_work_run(): one at a time, each under the radio's lock, taken before the next is looked for */
static void
works_run(struct work_struct *runner)
{
	for (;;) {
		struct wiphy_work *work = NULL;

		wiphy_lock(works.wiphy);
		lx_os_lock();
		if (!list_empty(&works.waiting)) {
			work = list_first_entry(&works.waiting, struct wiphy_work, entry);
			list_del_init(&work->entry);
		}
		lx_os_unlock();
		if (work != NULL) {
			work->fn(works.wiphy, work);
		}
		wiphy_unlock(works.wiphy);
		if (work == NULL) {
			return;
		}
	}
}

void
linuxkpi_wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work)
{
	if (!works.ready) {
		INIT_LIST_HEAD(&works.waiting);
		works.queue = alloc_ordered_workqueue("wifi-wiphy", 0);
		INIT_WORK(&works.runner, works_run);
		works.ready = true;
	}
	lx_os_lock();
	works.wiphy = wiphy;
	if (list_empty(&work->entry)) {
		list_add_tail(&work->entry, &works.waiting);
	}
	lx_os_unlock();
	queue_work(works.queue, &works.runner);
}

/* (called with the radio's lock held, as in Linux: what has not begun does not) */
void
linuxkpi_wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work)
{
	lx_os_lock();
	if (!list_empty(&work->entry)) {
		list_del_init(&work->entry);
	}
	lx_os_unlock();
}

/* (with the radio's lock held: what is waiting is run now) */
void
linuxkpi_wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work)
{
	bool waiting;

	lx_os_lock();
	waiting = !list_empty(&work->entry);
	if (waiting) {
		list_del_init(&work->entry);
	}
	lx_os_unlock();
	if (waiting) {
		work->fn(wiphy, work);
	}
}

/* the station of lx_80211.c never stops a queue */
bool
ieee80211_queue_stopped(struct ieee80211_hw *hw, int queue)
{
	return (false);
}

/* Bounded command-buffer timing. Included by redwoodmetal_commandbuffer.inc after the clock helpers.
 * All durations are microseconds from mach_absolute_time. Completion status comes from the existing
 * fence/error path; no timing threshold changes or predicts that status. */
#define RW_TIMING_FRAME_US 8333ULL
#define RW_TIMING_TEXTURE_US 2000ULL
#define RW_TIMING_SLOW_LINES 4

struct rw_timing_phase {
	uint64_t total, longest;
};

static pthread_mutex_t rw_timing_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	uint64_t commits, completions, completed, errors;
	struct rw_timing_phase encode, refresh, flush, queue, fence, writeback, total;
	unsigned slow_lines;
} rw_timing;

/* The caller holds rw_timing_lock. */
static void
rw_timing_add(struct rw_timing_phase *phase, uint64_t us)
{
	phase->total += us;
	phase->longest = us > phase->longest ? us : phase->longest;
}

static bool
rw_timing_slow_wanted(bool slow)
{
	if (!slow || rw_timing.slow_lines >= RW_TIMING_SLOW_LINES) {
		return false;
	}
	rw_timing.slow_lines++;
	return true;
}

static void
rw_timing_commit(unsigned commit, uint64_t encode_us, uint64_t refresh_us, uint64_t flush_us)
{
	bool say;

	pthread_mutex_lock(&rw_timing_lock);
	rw_timing.commits++;
	rw_timing_add(&rw_timing.encode, encode_us);
	rw_timing_add(&rw_timing.refresh, refresh_us);
	rw_timing_add(&rw_timing.flush, flush_us);
	say = rw_timing_slow_wanted(encode_us > RW_TIMING_FRAME_US ||
	    refresh_us > RW_TIMING_TEXTURE_US || flush_us > RW_TIMING_FRAME_US);
	pthread_mutex_unlock(&rw_timing_lock);
	if (say) {
		rw_print("tl: cb %u encode %llu refresh %llu flush %llu at %llu\n", commit,
		    (unsigned long long)encode_us, (unsigned long long)refresh_us,
		    (unsigned long long)flush_us, (unsigned long long)rw_now_us());
	}
}

static void
rw_timing_completion(unsigned commit, uint64_t queue_us, uint64_t fence_us, uint64_t writeback_us,
    bool completed)
{
	uint64_t total_us = queue_us + fence_us + writeback_us;
	bool say;

	pthread_mutex_lock(&rw_timing_lock);
	rw_timing.completions++;
	rw_timing.completed += completed;
	rw_timing.errors += !completed;
	rw_timing_add(&rw_timing.queue, queue_us);
	rw_timing_add(&rw_timing.fence, fence_us);
	rw_timing_add(&rw_timing.writeback, writeback_us);
	rw_timing_add(&rw_timing.total, total_us);
	say = rw_timing_slow_wanted(fence_us > RW_TIMING_FRAME_US ||
	    writeback_us > RW_TIMING_TEXTURE_US || total_us > RW_TIMING_FRAME_US);
	pthread_mutex_unlock(&rw_timing_lock);
	if (say) {
		rw_print("tl: cp %u queue %llu fence %llu writeback %llu total %llu status %s at %llu\n", commit,
		    (unsigned long long)queue_us, (unsigned long long)fence_us,
		    (unsigned long long)writeback_us, (unsigned long long)total_us,
		    completed ? "completed" : "error", (unsigned long long)rw_now_us());
	}
}

/* One line a second for an active process. The local copy lets recording continue outside the lock. */
static void
rw_timing_dump(uint64_t now)
{
	typeof(rw_timing) stats;

	pthread_mutex_lock(&rw_timing_lock);
	if (rw_timing.commits == 0 && rw_timing.completions == 0) {
		pthread_mutex_unlock(&rw_timing_lock);
		return;
	}
	stats = rw_timing;
	memset(&rw_timing, 0, sizeof(rw_timing));
	pthread_mutex_unlock(&rw_timing_lock);
	rw_print("tl: mt at %llu commits %llu encode %llu/%llu refresh %llu/%llu flush %llu/%llu "
	    "completions %llu completed %llu errors %llu queue %llu/%llu fence %llu/%llu "
	    "writeback %llu/%llu total %llu/%llu slow_lines %u\n",
	    (unsigned long long)now, (unsigned long long)stats.commits,
	    (unsigned long long)stats.encode.total, (unsigned long long)stats.encode.longest,
	    (unsigned long long)stats.refresh.total, (unsigned long long)stats.refresh.longest,
	    (unsigned long long)stats.flush.total, (unsigned long long)stats.flush.longest,
	    (unsigned long long)stats.completions, (unsigned long long)stats.completed,
	    (unsigned long long)stats.errors,
	    (unsigned long long)stats.queue.total, (unsigned long long)stats.queue.longest,
	    (unsigned long long)stats.fence.total, (unsigned long long)stats.fence.longest,
	    (unsigned long long)stats.writeback.total, (unsigned long long)stats.writeback.longest,
	    (unsigned long long)stats.total.total, (unsigned long long)stats.total.longest, stats.slow_lines);
}


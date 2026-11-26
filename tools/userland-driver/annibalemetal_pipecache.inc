/*
 * Render pipelines kept by what they were built from, included into annibalemetal.m.
 *
 * iconservicesagent drops every RenderBox pipeline after 3 s without an icon to draw
 * (-[ISGPUIdleTimer armTimer] arms a 3 s timer whose handler calls +[RBDevice purgeResources])
 * and builds the same 48 again for the next icon, each time the home screen comes back and the
 * Calendar and Clock icons are redrawn. Apple's driver gets them back from its compiler cache;
 * without one each is a translation and two ir3 compiles again. So a pipeline whose last owner
 * went away stays here, and a descriptor with the same modules, entry names and shape gets the
 * same library pipeline back (about 20 us instead of 1 to 6 ms).
 *
 * The key is a SHA-256 of those inputs. At most rw_pipe_idle_max() pipelines without an owner
 * are kept; past that the one idle longest is destroyed. The default was raised from 96 for the
 * prewarm replay (annibalemetal_prewarm.inc): backboardd's working set is measured at 230
 * pipelines a boot, and a cap below the working set evicts replayed pipelines during the replay
 * itself -- the ones replayed first are the ones idle longest. 384 covers the measured sets
 * (backboardd 230, cameracaptured 221) with headroom for transients; the memory it can pin is
 * the working set's own, tens of MB at the report's upper estimate, which is the price the
 * prewarm design already pays. ANNIBALE_PIPE_IDLE_MAX overrides it.
 */
#include <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#include <os/lock.h>
#include <pthread.h>

#define RW_PIPE_IDLE_MAX         384
#define RW_COMPUTE_PIPE_IDLE_MAX 2048
#define RW_COMPUTE_BUCKETS       256

static unsigned
rw_pipe_idle_max(void)
{
	static int configured = -1;

	if (configured < 0) {
		const char *env = getenv("ANNIBALE_PIPE_IDLE_MAX");

		configured = env != NULL ? atoi(env) : RW_PIPE_IDLE_MAX;
		if (configured < 1) {
			configured = 1;
		}
	}
	return (unsigned)configured;
}

struct rw_pipe_entry {
	unsigned char key[CC_SHA256_DIGEST_LENGTH];
	struct rwa_device *device;
	struct rwa_render *render;
	struct rwa_render_info info;
	unsigned users;
	uint64_t idle_since;          /* order of release, for choosing which idle one goes */
	struct rw_pipe_entry *next;
};

static struct rw_pipe_entry *rw_pipes;
static unsigned rw_pipes_idle;
static uint64_t rw_pipes_released;
static os_unfair_lock rw_pipes_lock = OS_UNFAIR_LOCK_INIT;

static void
rw_pipe_add_bytes(CC_SHA256_CTX *ctx, const void *bytes, size_t length)
{
	uint64_t n = length;

	CC_SHA256_Update(ctx, &n, sizeof(n));
	if (length != 0) {
		CC_SHA256_Update(ctx, bytes, (CC_LONG)length);
	}
}

/* The key over byte input rather than NSData, so the prewarm replay can key what it read out
 * of its file (annibalemetal_prewarm.inc) and the two sides hash the same pipeline the same way.
 * The whole shape goes in raw -- which is where the rgb2p-affecting colour formats enter the
 * key: lowering rewrites the fragment outputs by them, so two shapes differing only there must
 * not share a pipeline, and reducing the shape to the modules' hashes would collapse them. */
static void
rw_pipe_key_bytes(unsigned char key[CC_SHA256_DIGEST_LENGTH],
		  const void *vertex_air, size_t vertex_len, const char *vertex_name,
		  const void *fragment_air, size_t fragment_len, const char *fragment_name,
		  const struct rwa_render_shape *shape,
		  const struct rwa_linked_function *linked, unsigned num_linked)
{
	CC_SHA256_CTX ctx;

	CC_SHA256_Init(&ctx);
	rw_pipe_add_bytes(&ctx, vertex_air, vertex_len);
	rw_pipe_add_bytes(&ctx, vertex_name, vertex_name != NULL ? strlen(vertex_name) : 0);
	rw_pipe_add_bytes(&ctx, fragment_air, fragment_len);
	rw_pipe_add_bytes(&ctx, fragment_name, fragment_name != NULL ? strlen(fragment_name) : 0);
	rw_pipe_add_bytes(&ctx, shape, sizeof(*shape));
	rw_pipe_add_bytes(&ctx, &num_linked, sizeof(num_linked));
	for (unsigned i = 0; i < num_linked; i++) {
		rw_pipe_add_bytes(&ctx, linked[i].name, linked[i].name ? strlen(linked[i].name) : 0);
		rw_pipe_add_bytes(&ctx, &linked[i].output, sizeof(linked[i].output));
		rw_pipe_add_bytes(&ctx, &linked[i].num_nodes, sizeof(linked[i].num_nodes));
		for (unsigned j = 0; j < linked[i].num_nodes; j++) {
			const struct rwa_stitch_node *node = &linked[i].nodes[j];

			rw_pipe_add_bytes(&ctx, node->bitcode, node->size);
			rw_pipe_add_bytes(&ctx, node->function,
					  node->function ? strlen(node->function) : 0);
			rw_pipe_add_bytes(&ctx, &node->num_args, sizeof(node->num_args));
			if (node->num_args != 0 && node->args != NULL) {
				rw_pipe_add_bytes(&ctx, node->args,
						  (size_t)node->num_args * sizeof(node->args[0]));
			}
			rw_pipe_add_bytes(&ctx, &node->num_after, sizeof(node->num_after));
			if (node->num_after != 0 && node->after != NULL) {
				rw_pipe_add_bytes(&ctx, node->after,
						  (size_t)node->num_after * sizeof(node->after[0]));
			}
		}
	}
	CC_SHA256_Final(key, &ctx);
}

static void
rw_pipe_key(unsigned char key[CC_SHA256_DIGEST_LENGTH], NSData *vertex_air,
	    const char *vertex_name, NSData *fragment_air, const char *fragment_name,
	    const struct rwa_render_shape *shape,
	    const struct rwa_linked_function *linked, unsigned num_linked)
{
	rw_pipe_key_bytes(key, [vertex_air bytes], [vertex_air length], vertex_name,
			  [fragment_air bytes], [fragment_air length], fragment_name,
			  shape, linked, num_linked);
}

/* The kept pipeline for this key, with one more owner, or NULL. */
static struct rwa_render *
rw_pipe_find(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH],
	     struct rwa_render_info *info)
{
	struct rwa_render *render = NULL;

	os_unfair_lock_lock(&rw_pipes_lock);
	for (struct rw_pipe_entry *e = rw_pipes; e != NULL; e = e->next) {
		if (e->device == device && memcmp(e->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			if (e->users++ == 0) {
				rw_pipes_idle--;
			}
			*info = e->info;
			render = e->render;
			break;
		}
	}
	os_unfair_lock_unlock(&rw_pipes_lock);
	return render;
}

/* Whether the cache holds this key, without taking an owner the way rw_pipe_find does: the
 * prewarm replay only needs to know its work is already done. */
static bool
rw_pipe_present(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH])
{
	bool found = false;

	os_unfair_lock_lock(&rw_pipes_lock);
	for (struct rw_pipe_entry *e = rw_pipes; e != NULL; e = e->next) {
		if (e->device == device && memcmp(e->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			found = true;
			break;
		}
	}
	os_unfair_lock_unlock(&rw_pipes_lock);
	return found;
}

/*
 * In-flight creation exclusion, between the prewarm replay thread and the real request path.
 * Without it a request arriving while the replay is mid-build of the same key builds the
 * pipeline a second time -- the cost the replay exists to remove, paid twice at once. Whoever
 * enters first builds; the other side of the race behaves differently by design:
 *
 *   - the REQUEST thread waits in rw_pipe_inflight_enter itself, then re-reads the cache. The
 *     wait is bounded by a build already underway, which is strictly shorter than starting the
 *     same build from scratch, and the thread holds nothing the builder needs -- the exclusion
 *     is a leaf taken before any work starts, never held across the build.
 *   - the REPLAY thread skips the key: the request's own build populates the cache without it.
 *
 * A builder whose build fails leaves the key anyway; a waiter that then finds the cache still
 * empty builds without re-entering the protocol, so a pipeline that always refuses cannot loop
 * a request thread forever.
 */
struct rw_pipe_inflight {
	unsigned char key[CC_SHA256_DIGEST_LENGTH];
	struct rwa_device *device;
	struct rw_pipe_inflight *next;
};

static struct rw_pipe_inflight *rw_inflights;
static pthread_mutex_t rw_inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rw_inflight_cond = PTHREAD_COND_INITIALIZER;

/* True when the caller is the builder for this key; false when another thread's build of it
 * finished while the caller waited -- the caller must re-read the cache before building. */
static bool
rw_pipe_inflight_enter(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH])
{
	bool waited = false;

	pthread_mutex_lock(&rw_inflight_lock);
	for (;;) {
		struct rw_pipe_inflight *e;

		for (e = rw_inflights; e != NULL; e = e->next) {
			if (e->device == device &&
			    memcmp(e->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
				break;
			}
		}
		if (e == NULL) {
			if (waited) {
				break;
			}
			e = calloc(1, sizeof(*e));
			if (e == NULL) {
				/* out of memory degenerates to building unguarded, which is what
				 * every build was before the exclusion existed */
				pthread_mutex_unlock(&rw_inflight_lock);
				return true;
			}
			memcpy(e->key, key, CC_SHA256_DIGEST_LENGTH);
			e->device = device;
			e->next = rw_inflights;
			rw_inflights = e;
			pthread_mutex_unlock(&rw_inflight_lock);
			return true;
		}
		waited = true;
		pthread_cond_wait(&rw_inflight_cond, &rw_inflight_lock);
	}
	pthread_mutex_unlock(&rw_inflight_lock);
	return false;
}

static void
rw_pipe_inflight_leave(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH])
{
	pthread_mutex_lock(&rw_inflight_lock);
	for (struct rw_pipe_inflight **at = &rw_inflights; *at != NULL; at = &(*at)->next) {
		if ((*at)->device == device &&
		    memcmp((*at)->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			struct rw_pipe_inflight *e = *at;

			*at = e->next;
			free(e);
			break;
		}
	}
	pthread_cond_broadcast(&rw_inflight_cond);
	pthread_mutex_unlock(&rw_inflight_lock);
}

/* A pipeline just built, kept with its first owner. Out of memory leaves it unkept, and
 * rw_pipe_release then destroys it as before. */
static void
rw_pipe_keep(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH],
	     struct rwa_render *render, const struct rwa_render_info *info)
{
	struct rw_pipe_entry *e = calloc(1, sizeof(*e));

	if (e == NULL) {
		return;
	}
	memcpy(e->key, key, CC_SHA256_DIGEST_LENGTH);
	e->device = device;
	e->render = render;
	e->info = *info;
	e->users = 1;
	os_unfair_lock_lock(&rw_pipes_lock);
	for (struct rw_pipe_entry *cur = rw_pipes; cur != NULL; cur = cur->next) {
		if (cur->device == device && memcmp(cur->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			os_unfair_lock_unlock(&rw_pipes_lock);
			free(e);
			return;
		}
	}
	e->next = rw_pipes;
	rw_pipes = e;
	os_unfair_lock_unlock(&rw_pipes_lock);
}

/* An owner of this library pipeline went away. A kept one stays until it is the oldest of more
 * than rw_pipe_idle_max() idle ones; one that was never kept is destroyed now. The destroy runs
 * outside the lock because the library takes its own. */
static void
rw_pipe_release(struct rwa_render *render)
{
	struct rw_pipe_entry *e, *drop = NULL;
	bool kept = false;

	os_unfair_lock_lock(&rw_pipes_lock);
	for (e = rw_pipes; e != NULL; e = e->next) {
		if (e->render == render) {
			kept = true;
			if (--e->users == 0) {
				e->idle_since = ++rw_pipes_released;
				rw_pipes_idle++;
			}
			break;
		}
	}
	if (rw_pipes_idle > rw_pipe_idle_max()) {
		struct rw_pipe_entry **at, **oldest = NULL;

		for (at = &rw_pipes; *at != NULL; at = &(*at)->next) {
			if ((*at)->users == 0 &&
			    (oldest == NULL || (*at)->idle_since < (*oldest)->idle_since)) {
				oldest = at;
			}
		}
		drop = *oldest;
		*oldest = drop->next;
		rw_pipes_idle--;
	}
	os_unfair_lock_unlock(&rw_pipes_lock);
	if (!kept) {
		mesa.render_destroy(render);
	}
	if (drop != NULL) {
		/* eviction is the prewarm's failure mode made visible: a replayed pipeline
		 * destroyed before its first real request means the cap is under the working
		 * set, and the build it saved happens again after all */
		static BOOL said_evict;

		if (!said_evict) {
			said_evict = YES;
			rw_print("pipeline cache: more than %u idle pipelines, evicting the oldest; "
				 "ANNIBALE_PIPE_IDLE_MAX raises the cap\n", rw_pipe_idle_max());
		}
		mesa.render_destroy(drop->render);
		free(drop);
	}
}

/*
 * Compute pipeline cache.
 */
struct rw_compute_pipe_entry {
	unsigned char key[CC_SHA256_DIGEST_LENGTH];
	struct rwa_device *device;
	struct rwa_compute *compute;
	struct rwa_compute_info info;
	unsigned users;
	uint64_t idle_since;
	struct rw_compute_pipe_entry *next;
};

static struct rw_compute_pipe_entry *rw_compute_buckets[RW_COMPUTE_BUCKETS];
static unsigned rw_compute_pipes_idle;
static uint64_t rw_compute_pipes_released;
static os_unfair_lock rw_compute_pipes_lock = OS_UNFAIR_LOCK_INIT;
static char rw_air_sha_assoc_key;

static void
rw_sha256_bytes(const void *bytes, size_t length, unsigned char out[CC_SHA256_DIGEST_LENGTH])
{
	CC_SHA256_CTX sha;
	const unsigned char *p = (const unsigned char *)bytes;
	size_t left = length;

	CC_SHA256_Init(&sha);
	while (left != 0) {
		CC_LONG chunk = left > UINT32_MAX ? UINT32_MAX : (CC_LONG)left;
		CC_SHA256_Update(&sha, p, chunk);
		p += chunk;
		left -= chunk;
	}
	CC_SHA256_Final(out, &sha);
}

static void
rw_compute_pipe_key_from_sha(unsigned char key[CC_SHA256_DIGEST_LENGTH],
			     const unsigned char air_sha[CC_SHA256_DIGEST_LENGTH],
			     uint64_t air_len, const char *entry_name)
{
	CC_SHA256_CTX ctx;
	CC_SHA256_Init(&ctx);
	CC_SHA256_Update(&ctx, air_sha, CC_SHA256_DIGEST_LENGTH);
	CC_SHA256_Update(&ctx, &air_len, sizeof(air_len));
	rw_pipe_add_bytes(&ctx, entry_name, entry_name ? strlen(entry_name) : 0);
	CC_SHA256_Final(key, &ctx);
}

static void
rw_compute_pipe_key(unsigned char key[CC_SHA256_DIGEST_LENGTH], const void *air,
		    size_t air_len, const char *entry_name)
{
	unsigned char air_sha[CC_SHA256_DIGEST_LENGTH];

	rw_sha256_bytes(air, air_len, air_sha);
	rw_compute_pipe_key_from_sha(key, air_sha, (uint64_t)air_len, entry_name);
}

static void
rw_compute_pipe_key_nsdata(unsigned char key[CC_SHA256_DIGEST_LENGTH], NSData *air,
			   const char *entry_name)
{
	static Class mutable_data_cls;
	if (mutable_data_cls == Nil)
		mutable_data_cls = [NSMutableData class];

	if ([air isKindOfClass:mutable_data_cls]) {
		rw_compute_pipe_key(key, [air bytes], [air length], entry_name);
		return;
	}

	unsigned char air_sha[CC_SHA256_DIGEST_LENGTH];
	NSData *cached = objc_getAssociatedObject(air, &rw_air_sha_assoc_key);
	if (cached != nil && [cached length] == CC_SHA256_DIGEST_LENGTH) {
		memcpy(air_sha, [cached bytes], CC_SHA256_DIGEST_LENGTH);
	} else {
		rw_sha256_bytes([air bytes], [air length], air_sha);
		@autoreleasepool {
			NSData *d = [NSData dataWithBytes:air_sha length:CC_SHA256_DIGEST_LENGTH];
			objc_setAssociatedObject(air, &rw_air_sha_assoc_key, d,
						 OBJC_ASSOCIATION_RETAIN);
		}
	}
	rw_compute_pipe_key_from_sha(key, air_sha, (uint64_t)[air length], entry_name);
}

static struct rwa_compute *
rw_compute_pipe_find(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH],
		     struct rwa_compute_info *info)
{
	struct rwa_compute *compute = NULL;
	unsigned bucket = key[0] & (RW_COMPUTE_BUCKETS - 1u);

	os_unfair_lock_lock(&rw_compute_pipes_lock);
	for (struct rw_compute_pipe_entry *e = rw_compute_buckets[bucket]; e != NULL; e = e->next) {
		if (e->device == device && memcmp(e->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			if (e->users++ == 0) {
				rw_compute_pipes_idle--;
			}
			*info = e->info;
			compute = e->compute;
			break;
		}
	}
	os_unfair_lock_unlock(&rw_compute_pipes_lock);
	return compute;
}

static void
rw_compute_pipe_keep(struct rwa_device *device, const unsigned char key[CC_SHA256_DIGEST_LENGTH],
		     struct rwa_compute *compute, const struct rwa_compute_info *info)
{
	struct rw_compute_pipe_entry *e = calloc(1, sizeof(*e));
	if (e == NULL) return;
	unsigned bucket = key[0] & (RW_COMPUTE_BUCKETS - 1u);

	memcpy(e->key, key, CC_SHA256_DIGEST_LENGTH);
	e->device = device;
	e->compute = compute;
	e->info = *info;
	e->users = 1;
	os_unfair_lock_lock(&rw_compute_pipes_lock);
	for (struct rw_compute_pipe_entry *cur = rw_compute_buckets[bucket]; cur != NULL; cur = cur->next) {
		if (cur->device == device && memcmp(cur->key, key, CC_SHA256_DIGEST_LENGTH) == 0) {
			os_unfair_lock_unlock(&rw_compute_pipes_lock);
			free(e);
			return;
		}
	}
	e->next = rw_compute_buckets[bucket];
	rw_compute_buckets[bucket] = e;
	os_unfair_lock_unlock(&rw_compute_pipes_lock);
}

static void
rw_compute_pipe_release(struct rwa_compute *compute)
{
	struct rw_compute_pipe_entry *drop = NULL;
	bool kept = false;

	os_unfair_lock_lock(&rw_compute_pipes_lock);
	for (unsigned b = 0; b < RW_COMPUTE_BUCKETS && !kept; b++) {
		for (struct rw_compute_pipe_entry *e = rw_compute_buckets[b]; e != NULL; e = e->next) {
			if (e->compute == compute) {
				kept = true;
				if (--e->users == 0) {
					e->idle_since = ++rw_compute_pipes_released;
					rw_compute_pipes_idle++;
				}
				break;
			}
		}
	}
	if (rw_compute_pipes_idle > RW_COMPUTE_PIPE_IDLE_MAX) {
		struct rw_compute_pipe_entry **oldest = NULL;
		for (unsigned b = 0; b < RW_COMPUTE_BUCKETS; b++) {
			for (struct rw_compute_pipe_entry **at = &rw_compute_buckets[b];
			     *at != NULL; at = &(*at)->next) {
				if ((*at)->users == 0 &&
				    (oldest == NULL || (*at)->idle_since < (*oldest)->idle_since)) {
					oldest = at;
				}
			}
		}
		if (oldest != NULL) {
			drop = *oldest;
			*oldest = drop->next;
			rw_compute_pipes_idle--;
		}
	}
	os_unfair_lock_unlock(&rw_compute_pipes_lock);
	if (!kept) {
		mesa.compute_destroy(compute);
	}
	if (drop != NULL) {
		mesa.compute_destroy(drop->compute);
		free(drop);
	}
}

static void
rw_compute_pipe_forget(struct rwa_device *device)
{
	struct rw_compute_pipe_entry *mine = NULL;

	os_unfair_lock_lock(&rw_compute_pipes_lock);
	for (unsigned b = 0; b < RW_COMPUTE_BUCKETS; b++) {
		struct rw_compute_pipe_entry **at = &rw_compute_buckets[b];
		while (*at != NULL) {
			struct rw_compute_pipe_entry *e = *at;
			if (e->device == device && e->users == 0) {
				*at = e->next;
				e->next = mine;
				mine = e;
				rw_compute_pipes_idle--;
			} else {
				at = &e->next;
			}
		}
	}
	os_unfair_lock_unlock(&rw_compute_pipes_lock);
	while (mine != NULL) {
		struct rw_compute_pipe_entry *next = mine->next;
		mesa.compute_destroy(mine->compute);
		free(mine);
		mine = next;
	}
}

/* The device is closing: its idle pipelines go first, since the library destroys a pipeline
 * through the device it was made on. */
static void
rw_pipe_forget(struct rwa_device *device)
{
	struct rw_pipe_entry **at = &rw_pipes, *mine = NULL;

	os_unfair_lock_lock(&rw_pipes_lock);
	while (*at != NULL) {
		struct rw_pipe_entry *e = *at;

		if (e->device == device && e->users == 0) {
			*at = e->next;
			e->next = mine;
			mine = e;
			rw_pipes_idle--;
		} else {
			at = &e->next;
		}
	}
	os_unfair_lock_unlock(&rw_pipes_lock);
	while (mine != NULL) {
		struct rw_pipe_entry *next = mine->next;

		mesa.render_destroy(mine->render);
		free(mine);
		mine = next;
	}
}

/*
 * Render pipelines kept by what they were built from, included into redwoodmetal.m.
 *
 * iconservicesagent drops every RenderBox pipeline after 3 s without an icon to draw
 * (-[ISGPUIdleTimer armTimer] arms a 3 s timer whose handler calls +[RBDevice purgeResources])
 * and builds the same 48 again for the next icon, each time the home screen comes back and the
 * Calendar and Clock icons are redrawn. Apple's driver gets them back from its compiler cache;
 * without one each is a translation and two ir3 compiles again. So a pipeline whose last owner
 * went away stays here, and a descriptor with the same modules, entry names and shape gets the
 * same library pipeline back (about 20 us instead of 1 to 6 ms).
 *
 * The key is a SHA-256 of those inputs. Pipelines with linked fragment functions are not kept:
 * their inputs are a graph of nodes, and RenderBox builds four of them per round of icons.
 * At most RW_PIPE_IDLE_MAX pipelines without an owner are kept; past that the one idle longest
 * is destroyed, which bounds what a process that never asks twice (backboardd) holds on to.
 */
#include <CommonCrypto/CommonDigest.h>
#include <os/lock.h>

#define RW_PIPE_IDLE_MAX 96

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

static void
rw_pipe_key(unsigned char key[CC_SHA256_DIGEST_LENGTH], NSData *vertex_air,
	    const char *vertex_name, NSData *fragment_air, const char *fragment_name,
	    const struct rwa_render_shape *shape,
	    const struct rwa_linked_function *linked, unsigned num_linked)
{
	CC_SHA256_CTX ctx;

	CC_SHA256_Init(&ctx);
	rw_pipe_add_bytes(&ctx, [vertex_air bytes], [vertex_air length]);
	rw_pipe_add_bytes(&ctx, vertex_name, vertex_name != NULL ? strlen(vertex_name) : 0);
	rw_pipe_add_bytes(&ctx, [fragment_air bytes], [fragment_air length]);
	rw_pipe_add_bytes(&ctx, fragment_name, fragment_name != NULL ? strlen(fragment_name) : 0);
	rw_pipe_add_bytes(&ctx, shape, sizeof(*shape));
	rw_pipe_add_bytes(&ctx, &num_linked, sizeof(num_linked));
	for (unsigned i = 0; i < num_linked; i++) {
		rw_pipe_add_bytes(&ctx, linked[i].name, linked[i].name ? strlen(linked[i].name) : 0);
		rw_pipe_add_bytes(&ctx, &linked[i].output, sizeof(linked[i].output));
		rw_pipe_add_bytes(&ctx, &linked[i].num_nodes, sizeof(linked[i].num_nodes));
		for (unsigned j = 0; j < linked[i].num_nodes; j++) {
			rw_pipe_add_bytes(&ctx, linked[i].nodes[j].bitcode, linked[i].nodes[j].size);
			rw_pipe_add_bytes(&ctx, linked[i].nodes[j].function,
					  linked[i].nodes[j].function ? strlen(linked[i].nodes[j].function) : 0);
			rw_pipe_add_bytes(&ctx, &linked[i].nodes[j].num_args, sizeof(linked[i].nodes[j].num_args));
		}
	}
	CC_SHA256_Final(key, &ctx);
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
	e->next = rw_pipes;
	rw_pipes = e;
	os_unfair_lock_unlock(&rw_pipes_lock);
}

/* An owner of this library pipeline went away. A kept one stays until it is the oldest of more
 * than RW_PIPE_IDLE_MAX idle ones; one that was never kept is destroyed now. The destroy runs
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
	if (rw_pipes_idle > RW_PIPE_IDLE_MAX) {
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
		mesa.render_destroy(drop->render);
		free(drop);
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

/*
 * Pipeline harvest and prewarm replay, included into annibalemetal.m after the pipeline cache.
 *
 * Why this exists. CoreAnimation builds a pipeline in the frame that first needs it, and the
 * build costs about a millisecond of AIR->NIR translation on the thread that asked (backboardd
 * measured 1161 us average over 230 creations a boot, reusing only 25 of them), plus a first-draw
 * wait behind the compile queue. The set of pipelines a system process uses is closed -- the ir3
 * disk cache converged to zero new stores across boots -- so the set can be recorded once and
 * rebuilt before it is needed. Apple's own answer is the same shape: QuartzCore ships
 * default.pipelinelib with every pipeline precompiled, and on real hardware none of this is
 * built at runtime.
 *
 * The mechanism has two halves:
 *
 *   HARVEST. Every pipeline this process builds (not reuses) is appended to a per-process file
 *   in /private/var/db/annibale-pipeharvest/, as the exact inputs the build consumed: both AIR
 *   modules, both entry names, the shape verbatim and the linked-function graph. The key of the
 *   pipeline cache is a SHA-256 of exactly those inputs, so a record fed back verbatim produces
 *   the key a later real request will look up. The rgb2p lowering rewrites the fragment NIR by
 *   the colour formats of the shape; because the shape is recorded whole, those bits are in the
 *   key and in the replay with no extra dimension -- a key built from the AIR hash alone would
 *   collapse the rgb2p and plain variants of one module onto each other.
 *
 *   PREWARM. The first device of a process spawns one background thread that reads the file and
 *   rebuilds every record into the pipeline cache, in file order -- the order the pipelines were
 *   first needed in earlier boots, which is the best available guess at the order they will be
 *   needed in this one. Each rebuild is released as soon as it is kept, idle but resident: a
 *   later request of the same key finds it in microseconds instead of building. The build itself
 *   enqueues the shader variants on the ir3 compile queue, so by the time the first draw of the
 *   pipeline comes the binary is already retrieved and uploaded and the draw does not wait.
 *
 * Both halves are best-effort: a missing or unreadable file costs nothing but the prewarm's
 * absence, a record that fails to build is counted and skipped, and the file is input-side --
 * it holds what was ASKED, not what was compiled -- so a new compiler library build does not
 * invalidate it the way the ir3 disk cache's build UUID does. The format versions by the shape's
 * size instead: a driver built with a different struct rwa_render_shape truncates the file and
 * harvests again.
 *
 * ANNIBALE_HARVEST_OFF and ANNIBALE_PREWARM_OFF disable the halves; both default on.
 */
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#define RW_HARVEST_DIR "/private/var/db/annibale-pipeharvest"
#define RW_HARVEST_MAGIC "ANNHARV1"   /* 8 bytes; bump the digit when the record layout changes */
#define RW_HARVEST_REC_MAGIC 0x48455243u   /* 'HREC' little-endian */
#define RW_HARVEST_MAX_RECORDS 512
#define RW_HARVEST_MAX_BLOB (32u << 20)    /* one module or bitcode past this is corruption */
#define RW_HARVEST_MAX_NODES 1024          /* likewise; real stitching graphs hold a handful */

/* The file header: the magic, then the shape size the records were written with. A reader built
 * against another shape cannot trust the bytes and starts over. */
struct rw_harvest_header {
	char magic[8];
	uint32_t shape_size;
	uint32_t reserved;
};

/*
 * One record, parsed out of the file buffer. The modules and node bitcodes point into the
 * buffer; everything small is copied out -- the shape because the buffer's alignment is not the
 * struct's, the names because the file carries lengths and no terminators, and the integer
 * arrays because their alignment in the byte stream is only the writer's accident. Every copy
 * is on the record's allocation list, freed whole by rw_harvest_free_rec.
 */
struct rw_harvest_rec {
	unsigned char key[CC_SHA256_DIGEST_LENGTH];
	const uint8_t *vair, *fair;
	uint32_t vair_len, fair_len;
	char *vname, *fname;   /* NULL when the stage is absent, else NUL-terminated */
	struct rwa_render_shape shape;
	unsigned num_linked;
	struct rwa_linked_function linked[RW_LINKED_MAX];
	void **allocs;
	unsigned nallocs, alloc_cap;
};

static void *
rw_harvest_alloc(struct rw_harvest_rec *rec, size_t size)
{
	void *p = malloc(size != 0 ? size : 1);

	if (p == NULL) {
		return NULL;
	}
	if (rec->nallocs == rec->alloc_cap) {
		unsigned cap = rec->alloc_cap != 0 ? 2 * rec->alloc_cap : 16;
		void **grown = realloc(rec->allocs, cap * sizeof(*grown));

		if (grown == NULL) {
			free(p);
			return NULL;
		}
		rec->allocs = grown;
		rec->alloc_cap = cap;
	}
	rec->allocs[rec->nallocs++] = p;
	return p;
}

static void
rw_harvest_free_rec(struct rw_harvest_rec *rec)
{
	for (unsigned i = 0; i < rec->nallocs; i++) {
		free(rec->allocs[i]);
	}
	free(rec->allocs);
	rec->allocs = NULL;
	rec->nallocs = rec->alloc_cap = 0;
}

/* A counted string of the file into a NUL-terminated copy, or NULL for a zero length. */
static char *
rw_harvest_string(struct rw_harvest_rec *rec, const uint8_t *bytes, uint32_t length)
{
	if (length == 0) {
		return NULL;
	}
	char *copy = rw_harvest_alloc(rec, (size_t)length + 1);

	if (copy != NULL) {
		memcpy(copy, bytes, length);
		copy[length] = '\0';
	}
	return copy;
}

static uint32_t
rw_harvest_fnv1a(const uint8_t *bytes, size_t length)
{
	uint32_t hash = 2166136261u;

	for (size_t i = 0; i < length; i++) {
		hash = (hash ^ bytes[i]) * 16777619u;
	}
	return hash;
}

static uint32_t
rw_harvest_u32(const uint8_t *at)
{
	uint32_t value;

	memcpy(&value, at, sizeof(value));
	return value;
}

/*
 * The fixed part of one record: magic, total length, the key, the four module lengths and the
 * linked count -- 60 bytes. The variable parts follow in the writer's order (vertex name, vertex
 * module, fragment name, fragment module, shape, linked graph) and a four-byte fnv1a of
 * everything before it closes the record.
 */
#define RW_HARVEST_FIXED 60

/*
 * Parse the record at `at` (of `end`) into `rec`, materialising everything the creation call
 * needs. Returns the record's byte length when it is whole and its checksum agrees, zero when
 * it is torn or corrupt -- the caller treats that offset as the end of the usable file, because
 * appends never resume inside a record.
 */
static size_t
rw_harvest_parse(const uint8_t *at, const uint8_t *end, struct rw_harvest_rec *rec)
{
	memset(rec, 0, sizeof(*rec));
	if ((size_t)(end - at) < RW_HARVEST_FIXED + 4 || rw_harvest_u32(at) != RW_HARVEST_REC_MAGIC) {
		return 0;
	}
	uint32_t length = rw_harvest_u32(at + 4);

	if (length < RW_HARVEST_FIXED + 4 || length > (uint32_t)(end - at)) {
		return 0;
	}
	if (rw_harvest_fnv1a(at, length - 4) != rw_harvest_u32(at + length - 4)) {
		return 0;
	}
	memcpy(rec->key, at + 8, sizeof(rec->key));
	rec->vair_len = rw_harvest_u32(at + 40);
	uint32_t vname_len = rw_harvest_u32(at + 44);

	rec->fair_len = rw_harvest_u32(at + 48);
	uint32_t fname_len = rw_harvest_u32(at + 52);
	unsigned num_linked = rw_harvest_u32(at + 56);

	if (rec->vair_len > RW_HARVEST_MAX_BLOB || rec->fair_len > RW_HARVEST_MAX_BLOB ||
	    num_linked > RW_LINKED_MAX) {
		return 0;
	}
	const uint8_t *p = at + RW_HARVEST_FIXED, *stop = at + length - 4;

#define RW_HARVEST_TAKE(dst, len)						\
	do {									\
		if ((len) > (uint32_t)(stop - p)) {				\
			goto corrupt;						\
		}								\
		(dst) = p;							\
		p += (len);							\
	} while (0)
	const uint8_t *vname, *fname;

	RW_HARVEST_TAKE(vname, vname_len);
	RW_HARVEST_TAKE(rec->vair, rec->vair_len);
	RW_HARVEST_TAKE(fname, fname_len);
	RW_HARVEST_TAKE(rec->fair, rec->fair_len);
	if ((uint32_t)(stop - p) < sizeof(rec->shape)) {
		goto corrupt;
	}
	memcpy(&rec->shape, p, sizeof(rec->shape));
	p += sizeof(rec->shape);
	rec->vname = rw_harvest_string(rec, vname, vname_len);
	rec->fname = rw_harvest_string(rec, fname, fname_len);
	if ((vname_len != 0 && rec->vname == NULL) || (fname_len != 0 && rec->fname == NULL)) {
		goto corrupt;
	}
	rec->num_linked = num_linked;
	for (unsigned i = 0; i < num_linked; i++) {
		if ((uint32_t)(stop - p) < 12) {
			goto corrupt;
		}
		uint32_t name_len = rw_harvest_u32(p);
		uint32_t output = rw_harvest_u32(p + 4);
		uint32_t num_nodes = rw_harvest_u32(p + 8);
		const uint8_t *name;
		struct rwa_linked_function *fn = &rec->linked[i];

		p += 12;
		if (num_nodes == 0 || num_nodes > RW_HARVEST_MAX_NODES) {
			goto corrupt;
		}
		RW_HARVEST_TAKE(name, name_len);
		fn->name = rw_harvest_string(rec, name, name_len);
		if (name_len != 0 && fn->name == NULL) {
			goto corrupt;
		}
		fn->output = output;
		fn->num_nodes = num_nodes;
		fn->nodes = rw_harvest_alloc(rec, num_nodes * sizeof(*fn->nodes));
		if (fn->nodes == NULL) {
			goto corrupt;
		}
		/* the array is const in the contract; the cast is the parse filling it once */
		struct rwa_stitch_node *nodes = (struct rwa_stitch_node *)fn->nodes;

		memset(nodes, 0, num_nodes * sizeof(*nodes));
		for (unsigned j = 0; j < num_nodes; j++) {
			if ((uint32_t)(stop - p) < 16) {
				goto corrupt;
			}
			uint32_t function_len = rw_harvest_u32(p);
			uint32_t num_args = rw_harvest_u32(p + 4);
			uint32_t num_after = rw_harvest_u32(p + 8);
			uint32_t bitcode_len = rw_harvest_u32(p + 12);
			const uint8_t *function, *args, *after;

			p += 16;
			if (bitcode_len > RW_HARVEST_MAX_BLOB || num_args > 256 || num_after > 256) {
				goto corrupt;
			}
			RW_HARVEST_TAKE(function, function_len);
			RW_HARVEST_TAKE(args, 4 * num_args);
			RW_HARVEST_TAKE(after, 4 * num_after);
			RW_HARVEST_TAKE(nodes[j].bitcode, bitcode_len);
			if (bitcode_len == 0) {
				/* an early-return node carries no module, and the library reads
				 * the pointer and not the length for that */
				nodes[j].bitcode = NULL;
			}
			nodes[j].size = bitcode_len;
			nodes[j].function = rw_harvest_string(rec, function, function_len);
			if (function_len != 0 && nodes[j].function == NULL) {
				goto corrupt;
			}
			nodes[j].num_args = num_args;
			nodes[j].num_after = num_after;
			if (num_args != 0) {
				int *copy = rw_harvest_alloc(rec, 4 * (size_t)num_args);

				if (copy == NULL) {
					goto corrupt;
				}
				memcpy(copy, args, 4 * (size_t)num_args);
				nodes[j].args = copy;
			}
			if (num_after != 0) {
				unsigned *copy = rw_harvest_alloc(rec, 4 * (size_t)num_after);

				if (copy == NULL) {
					goto corrupt;
				}
				memcpy(copy, after, 4 * (size_t)num_after);
				nodes[j].after = copy;
			}
		}
	}
	if (p != stop) {
		goto corrupt;
	}
	return length;
corrupt:
	rw_harvest_free_rec(rec);
	memset(rec, 0, sizeof(*rec));
	return 0;
#undef RW_HARVEST_TAKE
}

/*
 * The harvest side. The file is appended once per key the process has ever built: the keys
 * already in it are read back at the first write, so a reboot does not duplicate what an
 * earlier boot recorded. Writes are one write() per record on the creating thread -- tens to
 * a few hundred microseconds of page-cache copy, once per pipeline per the file's lifetime,
 * against the millisecond of translation the record exists to remove.
 */
static int rw_harvest_fd = -1;
static bool rw_harvest_off, rw_harvest_ready;
static unsigned char (*rw_harvest_keys)[CC_SHA256_DIGEST_LENGTH];
static unsigned rw_harvest_nkeys;
static pthread_mutex_t rw_harvest_lock = PTHREAD_MUTEX_INITIALIZER;

/* The keys of every whole record in the file, and the file cut back to the last whole one:
 * a boot killed in the middle of an append leaves a torn tail, and appending after it would
 * bury the tear in the middle of the file where no reader could resynchronise. */
static void
rw_harvest_open(void)
{
	char path[256];

	if (getenv("ANNIBALE_HARVEST_OFF") != NULL) {
		rw_harvest_off = true;
		return;
	}
	snprintf(path, sizeof(path), "%s/%s.harvest", RW_HARVEST_DIR, getprogname());
	if (mkdir(RW_HARVEST_DIR, 0777) != 0 && errno != EEXIST) {
		/* the writers are unprivileged while /private/var/db is root's 0755: a mkdir
		 * failure means the skeleton did not pre-create the directory and the open
		 * below would only re-report it as ENOENT, hiding this errno */
		rw_print("prewarm harvest: cannot mkdir %s: %s\n", RW_HARVEST_DIR, strerror(errno));
		rw_harvest_off = true;
		return;
	}
	chmod(RW_HARVEST_DIR, 0777);   /* as the ir3 cache does: the writers differ in user */
	int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0666);

	if (fd < 0) {
		rw_print("prewarm harvest: cannot open %s: %s\n", path, strerror(errno));
		rw_harvest_off = true;
		return;
	}
	struct stat st;

	if (fstat(fd, &st) != 0) {
		close(fd);
		rw_harvest_off = true;
		return;
	}
	rw_harvest_keys = malloc(RW_HARVEST_MAX_RECORDS * sizeof(*rw_harvest_keys));
	if (rw_harvest_keys == NULL) {
		close(fd);
		rw_harvest_off = true;
		return;
	}
	off_t size = st.st_size, good = 0;
	bool header_ok = false;

	if (size >= (off_t)sizeof(struct rw_harvest_header)) {
		struct rw_harvest_header header;

		header_ok = pread(fd, &header, sizeof(header), 0) == (ssize_t)sizeof(header) &&
		    memcmp(header.magic, RW_HARVEST_MAGIC, 8) == 0 &&
		    header.shape_size == sizeof(struct rwa_render_shape);
	}
	if (header_ok) {
		/* the key scan reads ONLY the fixed part of each record and seeks past the
		 * variable part by the length in it -- never the whole file. Reading megabytes
		 * of bitcode here would put tens of milliseconds on the thread building this
		 * boot's first pipeline, which is the cost the whole mechanism exists to
		 * remove; the scan is a few hundred 60-byte reads. Framing alone validates
		 * the scan: a checksum failure inside a record costs at worst one undeduplicated
		 * append, and the replay side re-validates every byte before it replays. */
		uint8_t fixed[RW_HARVEST_FIXED];
		off_t at = sizeof(struct rw_harvest_header);

		good = at;
		while (at + RW_HARVEST_FIXED + 4 <= size &&
		       pread(fd, fixed, sizeof(fixed), at) == (ssize_t)sizeof(fixed) &&
		       rw_harvest_u32(fixed) == RW_HARVEST_REC_MAGIC) {
			uint32_t length = rw_harvest_u32(fixed + 4);

			if (length < RW_HARVEST_FIXED + 4 || length > RW_HARVEST_MAX_BLOB ||
			    at + length > size) {
				break;
			}
			if (rw_harvest_nkeys < RW_HARVEST_MAX_RECORDS) {
				memcpy(rw_harvest_keys[rw_harvest_nkeys++], fixed + 8,
				       CC_SHA256_DIGEST_LENGTH);
			}
			at += length;
			good = at;
		}
	}
	if (!header_ok && size != 0) {
		rw_print("prewarm harvest: %s is not this driver's format; harvesting again\n", path);
	}
	if (!header_ok || good != size) {
		if (ftruncate(fd, header_ok ? good : 0) != 0) {
			rw_print("prewarm harvest: cannot cut %s back: %s\n", path, strerror(errno));
			close(fd);
			rw_harvest_off = true;
			return;
		}
	}
	if (!header_ok) {
		struct rw_harvest_header header = { .shape_size = sizeof(struct rwa_render_shape) };

		memcpy(header.magic, RW_HARVEST_MAGIC, 8);
		if (pwrite(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
			rw_print("prewarm harvest: cannot write the header of %s: %s\n", path,
				 strerror(errno));
			close(fd);
			rw_harvest_off = true;
			return;
		}
	}
	rw_harvest_fd = fd;
	if (rw_harvest_nkeys != 0) {
		rw_print("prewarm harvest: %u pipelines harvested in earlier boots, %lld bytes in %s\n",
			 rw_harvest_nkeys, (long long)good, path);
	}
}

/*
 * One built pipeline into the harvest file, keyed exactly as the cache keys it: the record is
 * the inputs of the build verbatim, so what is replayed hashes to what a real request will ask
 * for. Called once per successful build, with the linked scratch still alive.
 */
static void
rw_harvest_record(const unsigned char key[CC_SHA256_DIGEST_LENGTH],
		  const void *vertex_air, size_t vertex_len, const char *vertex_name,
		  const void *fragment_air, size_t fragment_len, const char *fragment_name,
		  const struct rwa_render_shape *shape,
		  const struct rwa_linked_function *linked, unsigned num_linked)
{
	pthread_mutex_lock(&rw_harvest_lock);
	if (!rw_harvest_ready) {
		rw_harvest_ready = true;
		rw_harvest_open();
	}
	if (rw_harvest_off) {
		goto done;
	}
	for (unsigned i = 0; i < rw_harvest_nkeys; i++) {
		if (memcmp(rw_harvest_keys[i], key, CC_SHA256_DIGEST_LENGTH) == 0) {
			goto done;
		}
	}
	if (rw_harvest_nkeys >= RW_HARVEST_MAX_RECORDS) {
		static BOOL said;

		if (!said) {
			said = YES;
			rw_print("prewarm harvest: %u records is the cap; further pipelines are not "
				 "recorded\n", RW_HARVEST_MAX_RECORDS);
		}
		goto done;
	}
	{
		uint32_t vname_len = vertex_name != NULL ? (uint32_t)strlen(vertex_name) : 0;
		uint32_t fname_len = fragment_name != NULL ? (uint32_t)strlen(fragment_name) : 0;
		uint64_t wide = RW_HARVEST_FIXED + 4 + vname_len + vertex_len + fname_len +
		    fragment_len + sizeof(*shape);

		for (unsigned i = 0; i < num_linked; i++) {
			wide += 12 + (linked[i].name != NULL ? strlen(linked[i].name) : 0);
			for (unsigned j = 0; j < linked[i].num_nodes; j++) {
				const struct rwa_stitch_node *node = &linked[i].nodes[j];

				wide += 16 + (node->function != NULL ? strlen(node->function) : 0) +
				    4 * (uint64_t)node->num_args + 4 * (uint64_t)node->num_after + node->size;
			}
		}
		if (wide > RW_HARVEST_MAX_BLOB) {
			static BOOL said_big;

			if (!said_big) {
				said_big = YES;
				rw_print("prewarm harvest: a pipeline's inputs pass %u bytes; "
					 "records that large are not written\n", RW_HARVEST_MAX_BLOB);
			}
			goto done;
		}
		uint32_t length = (uint32_t)wide;
		uint8_t *record = malloc(length);

		if (record == NULL) {
			goto done;
		}
		uint8_t *p = record;
		uint32_t head[7];

		head[0] = RW_HARVEST_REC_MAGIC;
		head[1] = length;
		head[2] = (uint32_t)vertex_len;
		head[3] = vname_len;
		head[4] = (uint32_t)fragment_len;
		head[5] = fname_len;
		head[6] = num_linked;
		memcpy(p, head, 8);
		memcpy(p + 8, key, CC_SHA256_DIGEST_LENGTH);
		memcpy(p + 40, head + 2, sizeof(head) - 8);
		p += RW_HARVEST_FIXED;
		if (vname_len != 0) {
			memcpy(p, vertex_name, vname_len);
			p += vname_len;
		}
		if (vertex_len != 0) {
			memcpy(p, vertex_air, vertex_len);
			p += vertex_len;
		}
		if (fname_len != 0) {
			memcpy(p, fragment_name, fname_len);
			p += fname_len;
		}
		if (fragment_len != 0) {
			memcpy(p, fragment_air, fragment_len);
			p += fragment_len;
		}
		memcpy(p, shape, sizeof(*shape));
		p += sizeof(*shape);
		for (unsigned i = 0; i < num_linked; i++) {
			uint32_t name_len = linked[i].name != NULL ? (uint32_t)strlen(linked[i].name) : 0;
			uint32_t fields[3] = { name_len, linked[i].output, linked[i].num_nodes };

			memcpy(p, fields, sizeof(fields));
			p += sizeof(fields);
			if (name_len != 0) {
				memcpy(p, linked[i].name, name_len);
				p += name_len;
			}
			for (unsigned j = 0; j < linked[i].num_nodes; j++) {
				const struct rwa_stitch_node *node = &linked[i].nodes[j];
				uint32_t function_len =
				    node->function != NULL ? (uint32_t)strlen(node->function) : 0;
				uint32_t nodehead[4] = { function_len, node->num_args, node->num_after,
							 (uint32_t)node->size };

				memcpy(p, nodehead, sizeof(nodehead));
				p += sizeof(nodehead);
				if (function_len != 0) {
					memcpy(p, node->function, function_len);
					p += function_len;
				}
				if (node->num_args != 0 && node->args != NULL) {
					memcpy(p, node->args, 4 * (size_t)node->num_args);
					p += 4 * (size_t)node->num_args;
				}
				if (node->num_after != 0 && node->after != NULL) {
					memcpy(p, node->after, 4 * (size_t)node->num_after);
					p += 4 * (size_t)node->num_after;
				}
				if (node->size != 0) {
					memcpy(p, node->bitcode, node->size);
					p += node->size;
				}
			}
		}
		if ((uint32_t)(p - record) != length - 4) {
			/* the count and the writing disagree, which is a bug in this file rather
			 * than in the input: skipping the record costs one pipeline's prewarm */
			free(record);
			goto done;
		}
		uint32_t checksum = rw_harvest_fnv1a(record, length - 4);
		size_t written = 0;
		bool failed = false;

		memcpy(p, &checksum, 4);
		while (written < length && !failed) {
			ssize_t n = write(rw_harvest_fd, record + written, length - written);

			if (n <= 0) {
				failed = true;
			} else {
				written += (size_t)n;
			}
		}
		free(record);
		if (failed) {
			rw_print("prewarm harvest: write failed: %s; recording stops here\n",
				 strerror(errno));
			rw_harvest_off = true;
			goto done;
		}
		memcpy(rw_harvest_keys[rw_harvest_nkeys++], key, CC_SHA256_DIGEST_LENGTH);
		if ((rw_harvest_nkeys & (rw_harvest_nkeys - 1)) == 0) {
			rw_print("prewarm harvest: %u pipelines recorded\n", rw_harvest_nkeys);
		}
	}
done:
	pthread_mutex_unlock(&rw_harvest_lock);
}

/*
 * The prewarm side. One thread per process, started by the first device that can build: reads
 * the harvest file of this process and rebuilds every record into the pipeline cache. The
 * thread holds the device object for its own lifetime: the device closes the compiler library's
 * device in -dealloc, and a replay running behind it would be reading freed state. Metal's
 * default device in practice lives as long as the process; the hold only keeps that true for
 * the thread.
 */
struct rw_prewarm_job {
	id device_object;
	struct rwa_device *device;
};

static void *
rw_prewarm_thread(void *arg)
{
	struct rw_prewarm_job *job = arg;
	char path[256];
	uint64_t began = mach_absolute_time();

	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
	snprintf(path, sizeof(path), "%s/%s.harvest", RW_HARVEST_DIR, getprogname());
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		if (errno != ENOENT) {
			rw_print("prewarm: cannot read %s: %s\n", path, strerror(errno));
		}
		goto out;
	}
	{
		struct stat st;
		off_t size;

		if (fstat(fd, &st) != 0 || (size = st.st_size) < (off_t)sizeof(struct rw_harvest_header) ||
		    size >= (off_t)1 << 30) {
			close(fd);
			goto out;
		}
		uint8_t *bytes = malloc((size_t)size);

		if (bytes == NULL || pread(fd, bytes, (size_t)size, 0) != size) {
			free(bytes);
			close(fd);
			goto out;
		}
		close(fd);
		struct rw_harvest_header header;

		memcpy(&header, bytes, sizeof(header));
		if (memcmp(header.magic, RW_HARVEST_MAGIC, 8) != 0 ||
		    header.shape_size != sizeof(struct rwa_render_shape)) {
			free(bytes);
			goto out;
		}
		unsigned records = 0, replayed = 0, cached = 0, theirs = 0, failed = 0, bad = 0;
		const uint8_t *at = bytes + sizeof(struct rw_harvest_header), *end = bytes + size;

		while ((size_t)(end - at) >= RW_HARVEST_FIXED + 4) {
			struct rw_harvest_rec rec;
			size_t length = rw_harvest_parse(at, end, &rec);

			if (length == 0) {
				break;
			}
			at += length;
			records++;
			unsigned char key[CC_SHA256_DIGEST_LENGTH];

			/* the recomputed key, because the cache lookup and the in-flight exclusion
			 * both run on it: a record whose materialised inputs do not hash back to
			 * its key would populate the cache under a key nobody asks for */
			rw_pipe_key_bytes(key, rec.vair, rec.vair_len, rec.vname,
					  rec.fair, rec.fair_len, rec.fname,
					  &rec.shape, rec.num_linked != 0 ? rec.linked : NULL,
					  rec.num_linked);
			if (memcmp(key, rec.key, sizeof(key)) != 0) {
				bad++;
				rw_harvest_free_rec(&rec);
				continue;
			}
			if (rw_pipe_present(job->device, key)) {
				cached++;
				rw_harvest_free_rec(&rec);
				continue;
			}
			if (!rw_pipe_inflight_enter(job->device, key)) {
				/* a real request is building or just built this one: the cache is
				 * or will be served without this thread's copy of the work */
				theirs++;
				rw_harvest_free_rec(&rec);
				continue;
			}
			struct rwa_render_info info = { 0 };
			char why[256] = { 0 };
			struct rwa_render *render = NULL;
			/* a stage the record does not carry is a NULL module pointer, matching
			 * the request path's nil NSData: the library reads a non-NULL pointer as
			 * a stage present, whatever its length */
			const void *vair = rec.vair_len != 0 ? rec.vair : NULL;
			const void *fair = rec.fair_len != 0 ? rec.fair : NULL;

			/* Step aside briefly when a foreground commit is active so prewarm does
			 * not contend for CPU cores with SpringBoard/backboardd frame submission;
			 * zero wait when commit_in_flight == 0. */
			for (unsigned tries = 0;
			     tries < 4u &&
			     __builtin_expect(__atomic_load_n(&commit_in_flight, __ATOMIC_ACQUIRE) != 0, 0);
			     tries++) {
				usleep(2000);
			}

			if (rec.num_linked != 0 && mesa.render_create_linked == NULL) {
				/* a record with a linked graph cannot be built without the linked
				 * entry point: caching the graphless build under the graph's key
				 * would answer a real request with the wrong shader */
				snprintf(why, sizeof(why), "the compiler library has no linked creation");
			} else {
				render = mesa.render_create_linked != NULL
				    ? mesa.render_create_linked(job->device, vair, rec.vair_len,
					  rec.vname, fair, rec.fair_len, rec.fname,
					  rec.linked, rec.num_linked, &rec.shape, &info, why, sizeof(why))
				    : mesa.render_create(job->device, vair, rec.vair_len, rec.vname,
					  fair, rec.fair_len, rec.fname, &rec.shape, &info,
					  why, sizeof(why));
			}
			if (render != NULL) {
				rw_pipe_keep(job->device, key, render, &info);
			}
			/* as in the request path: the keep precedes the leave, or a woken waiter
			 * reads an empty cache and builds the pipeline a second time */
			rw_pipe_inflight_leave(job->device, key);
			if (render != NULL) {
				rw_pipe_release(render);
				replayed++;
			} else {
				failed++;
				if (failed <= 4) {
					rw_print("prewarm: a replayed pipeline is refused: %s\n", why);
				}
			}
			rw_harvest_free_rec(&rec);
		}
		free(bytes);
		if (records != 0) {
			struct mach_timebase_info tb;
			uint64_t ms;

			mach_timebase_info(&tb);
			ms = (mach_absolute_time() - began) * tb.numer / tb.denom / 1000000;
			rw_print("prewarm: %u replayed, %u already cached, %u left to their callers, "
				 "%u refused, %u corrupt of %u harvested in %llu ms\n",
				 replayed, cached, theirs, failed, bad, records,
				 (unsigned long long)ms);
		}
	}
out:
	[job->device_object release];
	free(job);
	return NULL;
}

/*
 * The once-per-process start, from the first device that can build pipelines. Pipelines are
 * cached per device, so a second device of the same process is not warmed -- Metal's default
 * device is in practice the only one, and a process that makes more pays its builds as before.
 * The thread is detached and the device object retained into it; a process without a harvest
 * file pays one open() and nothing else.
 */
static void
rw_prewarm_start(AnnibaleMetalDevice *device_object, struct rwa_device *device)
{
	static bool started;

	if (getenv("ANNIBALE_PREWARM_OFF") != NULL) {
		return;
	}
	pthread_mutex_lock(&rw_harvest_lock);
	if (started) {
		pthread_mutex_unlock(&rw_harvest_lock);
		return;
	}
	started = true;
	pthread_mutex_unlock(&rw_harvest_lock);
	struct rw_prewarm_job *job = calloc(1, sizeof(*job));
	pthread_t thread;

	if (job == NULL) {
		return;
	}
	job->device_object = [device_object retain];
	job->device = device;
	if (pthread_create(&thread, NULL, rw_prewarm_thread, job) != 0) {
		[job->device_object release];
		free(job);
		return;
	}
	pthread_detach(thread);
}

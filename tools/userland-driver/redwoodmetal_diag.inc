/*
 * Diagnostics of the command buffer path that only print: where the threads of a stalled process
 * are, the switch that waits after every draw, and the shaders of the draw that did not finish.
 * Included by redwoodmetal.m after air_dump and before the pipelines and command buffers that
 * call these.
 */
#include <mach/mach.h>
#include <pthread.h>
#include "redwoodmetal_work_private.h"

/*
 * REDWOOD_SYNC_DRAWS=1 (package.py SYNC_JOBS): every draw is flushed and waited for before the
 * next one is recorded. A GPU hang closes submissions for the rest of the boot and the kernel names
 * only the process and its submission, which holds dozens of draws; waiting after each one names
 * the draw that did not finish. Slow, so it is for the process under suspicion only.
 */
static bool
rw_sync_draws(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("REDWOOD_SYNC_DRAWS") != NULL;
	return on != 0;
}

/* With rw_sync_draws, each render pipeline's modules are kept, so that the draw that does not
 * finish can have its shaders printed (air_dump, uncounted; airfail-extract.py reads them back,
 * stage names "hung-vertex" and "hung-fragment"). The modules are what the stitching made at run
 * time, so the log is the only place they can be read from. */
#define RW_KEPT_PIPELINES 256
static struct {
	const void *render;
	NSData *vertex, *fragment;
	char vertex_name[96], fragment_name[96];
} rw_kept[RW_KEPT_PIPELINES];
static unsigned rw_kept_count;

static void
rw_keep_air(const void *render, NSData *vertex, const char *vertex_name, NSData *fragment,
	    const char *fragment_name)
{
	if (!rw_sync_draws() || render == NULL || rw_kept_count >= RW_KEPT_PIPELINES)
		return;
	rw_kept[rw_kept_count].render = render;
	rw_kept[rw_kept_count].vertex = [vertex copy];
	rw_kept[rw_kept_count].fragment = [fragment copy];
	snprintf(rw_kept[rw_kept_count].vertex_name, sizeof(rw_kept[0].vertex_name), "%s",
		 vertex_name ? vertex_name : "(none)");
	snprintf(rw_kept[rw_kept_count].fragment_name, sizeof(rw_kept[0].fragment_name), "%s",
		 fragment_name ? fragment_name : "(none)");
	rw_kept_count++;
}

/*
 * REDWOOD_THUMBS=N, with REDWOOD_SYNC_DRAWS: from commit REDWOOD_THUMB_FROM on (default 1), a
 * 32x32 sample of every texture a commit drew into or sampled, once each when the commit ends
 * (reported as work 0), N samples in all. REDWOOD_THUMB_DRAWS=1 samples each draw instead: its
 * attachments before it ("pre"), its attachments and textures after it; REDWOOD_THUMB_MATCH=TEXT
 * limits that to pipelines whose vertex or fragment function name contains TEXT.
 * Each sample is a header line and 32 lines of 32 texels, four characters a texel: R, G, B and A
 * in their top six bits, as digits of rw_thumb_digits. tools/k90auto/thumbs.py turns them back
 * into images. Each line waits a millisecond after it is printed: the log ring holds about 1.3 MB
 * between two pulls, and a commit's samples printed at once overran it.
 */
#define RW_THUMB 32
static const char rw_thumb_digits[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int rw_thumbs_left = -1;
static unsigned rw_thumb_from;
static bool rw_thumb_draws;
static const char *rw_thumb_text;

static bool
rw_thumbs_wanted(unsigned commit)
{
	if (rw_thumbs_left < 0) {
		const char *count = getenv("REDWOOD_THUMBS"), *from = getenv("REDWOOD_THUMB_FROM");

		rw_thumbs_left = count != NULL ? atoi(count) : 0;
		rw_thumb_from = from != NULL ? (unsigned)atoi(from) : 1;
		rw_thumb_draws = getenv("REDWOOD_THUMB_DRAWS") != NULL;
		rw_thumb_text = getenv("REDWOOD_THUMB_MATCH");
	}
	return rw_sync_draws() && rw_thumbs_left > 0 && commit >= rw_thumb_from;
}

/* The first words of a buffer a sampled draw binds, in hex, 24 a line: RenderBox's uniforms hold
 * half4 colours, which a print of floats does not show. REDWOOD_THUMB_WORDS=N prints N words
 * (default 24, at most 4096); each line names its own offset. */
static void
rw_thumb_words(const char *stage, unsigned slot, id buffer, size_t offset, unsigned commit,
	       unsigned work)
{
	static int words = -1;
	const uint8_t *base = buffer != nil ? (const uint8_t *)[(id<MTLBuffer>)buffer contents] : NULL;
	size_t length = buffer != nil ? [(id<MTLBuffer>)buffer length] : 0;

	if (words < 0) {
		const char *env = getenv("REDWOOD_THUMB_WORDS");

		words = env != NULL ? atoi(env) : 24;
		words = words < 1 ? 24 : words > 4096 ? 4096 : words;
	}
	if (base == NULL || offset >= length)
		return;
	for (size_t line = 0; line < (size_t)words && offset + 4 * line < length; line += 24) {
		char text[24 * 9 + 1] = "";
		size_t at = 0, from = offset + 4 * line;

		for (size_t k = 0; k < 24 && line + k < (size_t)words && from + 4 * k + 4 <= length; k++) {
			uint32_t word;

			memcpy(&word, base + from + 4 * k, sizeof(word));
			at += (size_t)snprintf(text + at, sizeof(text) - at, " %08x", word);
		}
		rw_print("thumb %s buffer %u of commit %u work %u at %zu:%s\n", stage, slot, commit,
		    work, from, text);
		usleep(1000);
	}
}

/* Whether a draw of this pipeline is sampled under REDWOOD_THUMB_MATCH; the names are the ones
 * rw_keep_air kept, so a pipeline that was not kept does not match. */
static bool
rw_thumb_match(const void *render)
{
	if (rw_thumb_text == NULL)
		return true;
	for (unsigned i = 0; i < rw_kept_count; i++)
		if (rw_kept[i].render == render)
			return strstr(rw_kept[i].vertex_name, rw_thumb_text) != NULL ||
			    strstr(rw_kept[i].fragment_name, rw_thumb_text) != NULL;
	return false;
}

/* REDWOOD_THUMB_WHOLE=1 widens REDWOOD_THUMB_MATCH from a draw to its commit: every draw of a
 * commit that has a matching draw is sampled, so what consumes a path's winding is seen too.
 * Asked once per commit, before its first draw runs. */
static unsigned rw_thumb_whole_commit;

static void
rw_thumb_commit(NSArray *works, unsigned commit)
{
	static int whole = -1;

	if (whole < 0)
		whole = getenv("REDWOOD_THUMB_WHOLE") != NULL;
	if (!whole || !rw_thumbs_wanted(commit))
		return;
	for (RedwoodMetalWork *work in works)
		if (work->render != nil && rw_thumb_match(((void *(*)(id, SEL))objc_msgSend)(
		    work->render, sel_registerName("render")))) {
			rw_thumb_whole_commit = commit;
			return;
		}
}

static bool
rw_thumb_work_wanted(const void *render, unsigned commit)
{
	if (rw_thumb_whole_commit != 0)
		return rw_thumb_whole_commit == commit;
	return rw_thumb_match(render);
}

static void
rw_thumb(struct rwa_texture *texture, id object, unsigned commit, unsigned work, const char *role,
	 unsigned slot)
{
	static unsigned seq;
	uint8_t rgba[RW_THUMB * RW_THUMB * 4];
	char row[RW_THUMB * 4 + 1];

	if (texture == NULL || rw_thumbs_left <= 0 || mesa.texture_thumbnail == NULL ||
	    mesa.texture_thumbnail(texture, RW_THUMB, RW_THUMB, rgba) != 0)
		return;
	rw_thumbs_left--;
	rw_print("thumb %u: commit %u work %u %s %u: format %lu %lux%lu %p%s\n", seq, commit, work,
	    role, slot, (unsigned long)[(id<MTLTexture>)object pixelFormat],
	    (unsigned long)[(id<MTLTexture>)object width],
	    (unsigned long)[(id<MTLTexture>)object height], (void *)texture,
	    [(id<MTLTexture>)object buffer] != nil ? " buffer" :
	    [(id<MTLTexture>)object iosurface] != NULL ? " surface" : "");
	for (unsigned y = 0; y < RW_THUMB; y++) {
		for (unsigned x = 0; x < RW_THUMB * 4; x++)
			row[x] = rw_thumb_digits[rgba[y * RW_THUMB * 4 + x] >> 2];
		row[RW_THUMB * 4] = '\0';
		rw_print("thumb %u %02u: %s\n", seq, y, row);
		usleep(1000);
	}
	seq++;
}

/* One attachment: Metal pixel format, size, sample count, whether it is over an IOSurface (linear)
 * or the driver's own memory, and whether the pass clears it. */
static size_t
rw_describe_attachment(id texture, BOOL clears, char *out, size_t size)
{
	if (texture == nil)
		return (size_t)snprintf(out, size, " -");
	return (size_t)snprintf(out, size, " %lu %lux%lu s%lu %s%s",
	    (unsigned long)[(id<MTLTexture>)texture pixelFormat],
	    (unsigned long)[(id<MTLTexture>)texture width],
	    (unsigned long)[(id<MTLTexture>)texture height],
	    (unsigned long)[(id<MTLTexture>)texture sampleCount],
	    [(id<MTLTexture>)texture iosurface] != NULL ? "surface" : "own",
	    clears ? " clear" : "");
}

/* What a draw was, in sync mode: kept for the draw that does not finish, and said before it is
 * handed to the GPU when `say` -- with REDWOOD_SYNC_LOG, or for a commit REDWOOD_THUMBS samples.
 * Saying it for every draw was 35838 lines and 10 MB of iconservicesagent's log in render62, more
 * than the 1.3 MB ring holds between two pulls, and the start of the icon work was lost. */
static char rw_last_draw[768];

static bool
rw_sync_log_wanted(unsigned commit)
{
	static int wanted = -1;

	if (wanted < 0)
		wanted = getenv("REDWOOD_SYNC_LOG") != NULL;
	return wanted || rw_thumbs_wanted(commit);
}

static void
rw_describe_draw(RedwoodMetalWork *work, const void *render, unsigned commit, unsigned index,
		 bool say)
{
	char text[768];
	size_t at = (size_t)snprintf(text, sizeof(text),
	    "sync: commit %u work %u: pipeline %p prim %u vertices %u+%u instances %u%s, colour",
	    commit, index, render, work->primitive, work->vertexStart,
	    work->vertexCount, work->instanceCount, work->indexBuffer != nil ? " indexed" : "");
	for (unsigned i = 0; i < work->colorCount && at < sizeof(text); i++)
		at += rw_describe_attachment(work->color[i], work->clears, text + at, sizeof(text) - at);
	if (at < sizeof(text)) {
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", depth");
	}
	if (at < sizeof(text))
		at += rw_describe_attachment(work->depth, work->clearsDepth, text + at, sizeof(text) - at);
	if (at < sizeof(text) && work->clears)
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", clear colour %g %g %g %g",
		    work->clearColor[0], work->clearColor[1], work->clearColor[2], work->clearColor[3]);
	if (at < sizeof(text) && work->clearsDepth)
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", clear depth %g", work->clearDepth);
	if (at < sizeof(text) && work->depthStencilState != nil) {
		/* the four answers of <MTLDepthStencilStateSPI>, sent by name because the class is
		 * declared after this file */
		BOOL (*ask)(id, SEL) = (BOOL (*)(id, SEL))objc_msgSend;
		id state = work->depthStencilState;
		at += (size_t)snprintf(text + at, sizeof(text) - at, " test %s%s%s%s",
		    ask(state, sel_registerName("readsDepth")) ? "r" : "-",
		    ask(state, sel_registerName("writesDepth")) ? "w" : "-",
		    ask(state, sel_registerName("readsStencil")) ? "r" : "-",
		    ask(state, sel_registerName("writesStencil")) ? "w" : "-");
		if ([state respondsToSelector:sel_registerName("redwoodDepthCompare")])
			at += (size_t)snprintf(text + at, sizeof(text) - at, " cmp %u",
			    ((unsigned (*)(id, SEL))objc_msgSend)(state,
			    sel_registerName("redwoodDepthCompare")));
	}
	if (at < sizeof(text) && work->hasViewportRect)
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", viewport %.0f,%.0f %.0fx%.0f",
		    work->viewportRect[0], work->viewportRect[1], work->viewportRect[2],
		    work->viewportRect[3]);
	if (at < sizeof(text) && work->hasScissorRect)
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", scissor %u,%u %ux%u",
		    work->scissorRect[0], work->scissorRect[1], work->scissorRect[2],
		    work->scissorRect[3]);
	if (at < sizeof(text) && work->indexBuffer != nil) {
		/* the first indices as drawn: Metal's index type 0 is 16-bit, 1 is 32-bit */
		const uint8_t *base = (const uint8_t *)[(id<MTLBuffer>)work->indexBuffer contents];
		unsigned size = work->indexType == 0 ? 2 : 4;
		at += (size_t)snprintf(text + at, sizeof(text) - at, ", indices at %zu:",
		    work->indexBufferOffset);
		for (unsigned i = 0; base != NULL && i < work->vertexCount && i < 12 && at < sizeof(text); i++) {
			const uint8_t *p = base + work->indexBufferOffset + (size_t)i * size;
			unsigned v = size == 2 ? (unsigned)(p[0] | p[1] << 8) :
			    (unsigned)(p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24);
			at += (size_t)snprintf(text + at, sizeof(text) - at, " %u", v);
		}
	}
	if (at < sizeof(text))
		snprintf(text + at, sizeof(text) - at, ", %u textures, %u buffers", work->textureCount,
		    work->count);
	strlcpy(rw_last_draw, text, sizeof(rw_last_draw));
	if (say)
		rw_print("%s\n", text);
}

/* `why` names the dump: "hung" for the draw the GPU did not finish, "refused" for a draw the
 * library would not run (stage names "hung-fragment", "refused-fragment" and so on). */
static void
rw_dump_kept_air(const void *render, const char *why)
{
	for (unsigned i = 0; i < rw_kept_count; i++) {
		if (rw_kept[i].render != render)
			continue;
		char fragment[32], vertex[32];
		snprintf(fragment, sizeof(fragment), "%s-fragment", why);
		snprintf(vertex, sizeof(vertex), "%s-vertex", why);
		/* Fragment first: the process can be killed before the dump ends, and the hangs
		 * so far came down to the fragment shader. */
		air_dump([rw_kept[i].fragment bytes], [rw_kept[i].fragment length], fragment,
			 rw_kept[i].fragment_name, false);
		air_dump([rw_kept[i].vertex bytes], [rw_kept[i].vertex length], vertex,
			 rw_kept[i].vertex_name, false);
		return;
	}
	rw_print("sync: pipeline %p was not kept, so its shaders cannot be printed\n", render);
}

/* The shaders of a pipeline whose draw the library refused, once per pipeline: the refusal names
 * a binding, and only the module says how the shader declares it. */
static void
rw_dump_refused_air(const void *render)
{
	static const void *dumped[16];
	static unsigned count;

	if (!rw_sync_draws())
		return;
	for (unsigned i = 0; i < count; i++)
		if (dumped[i] == render)
			return;
	if (count == sizeof(dumped) / sizeof(dumped[0]))
		return;
	dumped[count++] = render;
	rw_dump_kept_air(render, "refused");
}

/* "image+0xoffset" for an address inside a loaded image, the bare address otherwise. The offset is
 * from the image's own load address, so a library of ours is read with atos -l 0 and a shared cache
 * image by adding the base dsc.py images prints. */
static int
rw_where(char *out, size_t size, uintptr_t address)
{
	Dl_info where;
	int n;

	if (address != 0 && dladdr((void *)address, &where) != 0 && where.dli_fname != NULL) {
		const char *name = strrchr(where.dli_fname, '/');

		n = snprintf(out, size, " %s+0x%lx", name != NULL ? name + 1 : where.dli_fname,
		    (unsigned long)(address - (uintptr_t)where.dli_fbase));
	} else {
		n = snprintf(out, size, " %p", (void *)address);
	}
	/* the count written, so the caller's position never passes the end of its buffer */
	return n < 0 ? 0 : n >= (int)size ? (int)size - 1 : n;
}

/* Where the threads of this process were when a draw had run for a third of a second, or when a
 * commit stopped moving. The kernel's kern.redwood.thread_dump refuses a write from backboardd and
 * SpringBoard, which run as mobile; a process may read its own threads' registers without that, so
 * the snapshot is taken here instead. Per thread one line with the registers and one with pc, lr and
 * up to 16 return addresses from the frame chain. The chain is followed only inside the thread's own
 * stack, which is mapped for as long as the thread exists, and the thread keeps running while it is
 * read, so the deepest entries can belong to a frame that has already returned. */
static void
rw_print_threads(void)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		rw_print("redwood: stall: cannot list the threads of this process\n");
		return;
	}
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		thread_identifier_info_data_t ident;
		mach_msg_type_number_t ident_count = THREAD_IDENTIFIER_INFO_COUNT;
		uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
		const char *image = "?";
		unsigned long long tid = 0;
		char frames[768];
		int used = 0;

		frames[0] = '\0';
		/* arm64e keeps the pointer fields opaque, so they come out through the accessors. */
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state,
		    &state_count) == KERN_SUCCESS) {
			Dl_info where;
			pthread_t posix = pthread_from_mach_thread_np(threads[i]);
			uintptr_t top = posix != NULL ? (uintptr_t)pthread_get_stackaddr_np(posix) : 0;
			uintptr_t bottom = posix != NULL ? top - pthread_get_stacksize_np(posix) : 0;

			pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(state);
			lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(state);
			fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(state);
			sp = (uintptr_t)__darwin_arm_thread_state64_get_sp(state);
			if (pc != 0 && dladdr((void *)pc, &where) != 0 && where.dli_fname != NULL)
				image = where.dli_fname;
			used += rw_where(frames + used, sizeof(frames) - (size_t)used, pc);
			used += rw_where(frames + used, sizeof(frames) - (size_t)used, lr);
			for (uintptr_t at = fp, depth = 0; depth < 16 && (at & 7) == 0 && at >= bottom &&
			    at + 16 <= top && used < (int)sizeof(frames) - 64; depth++) {
				uintptr_t next = ((const uintptr_t *)at)[0];
				uintptr_t back = (uintptr_t)ptrauth_strip(((void *const *)at)[1],
				    ptrauth_key_return_address);

				used += rw_where(frames + used, sizeof(frames) - (size_t)used, back);
				if (next <= at)
					break;
				at = next;
			}
		}
		if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &ident_count) ==
		    KERN_SUCCESS)
			tid = (unsigned long long)ident.thread_id;
		rw_print("redwood: stall pc %p lr %p fp %p sp %p (%s) tid %llu\n", (void *)pc, (void *)lr,
		    (void *)fp, (void *)sp, image, tid);
		rw_print("redwood: stall frames tid %llu:%s\n", tid, frames);
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
}

/*
 * REDWOOD_DUMP_AIR=TEXT: the bitcode -[_MTLFunction bitcodeData] gives this driver for the
 * functions of a render pipeline whose name contains TEXT, once per name and at most 16 of them,
 * base64 in lines of 96 characters: `air dump NAME: N bytes`, then `air dump NAME K: ...`. A
 * specialized function's bitcode is what _MTLLibrary made in process, which no file on disk holds;
 * tools/k90auto/airdump.py writes the modules back out for llvm-dis. The lines are paced at one
 * per 2 ms: dumping 48 modules at once in render59 lost the first 38 pipelines' log lines.
 */
static void
rw_dump_air(id function)
{
	static NSMutableSet *seen;
	static unsigned left = 16;
	enum { LINE = 96 };
	const char *text_wanted = getenv("REDWOOD_DUMP_AIR");
	SEL bitcode = sel_getUid("bitcodeData");

	if (function == nil || ![function respondsToSelector:bitcode])
		return;
	@synchronized ([NSData class]) {
		NSString *name = [function name];
		NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

		if (left == 0 || name == nil || air == nil || text_wanted == NULL ||
		    strstr([name UTF8String], text_wanted) == NULL || [seen containsObject:name])
			return;
		if (seen == nil)
			seen = [[NSMutableSet alloc] init];
		[seen addObject:name];
		left--;

		const char *text = [[air base64EncodedStringWithOptions:0] UTF8String];
		size_t length = strlen(text);

		rw_print("air dump %s: %lu bytes\n", [name UTF8String], (unsigned long)[air length]);
		for (size_t at = 0; at < length; at += LINE) {
			rw_print("air dump %s %zu: %.*s\n", [name UTF8String], at / LINE,
			    (int)MIN((size_t)LINE, length - at), text + at);
			usleep(2000);
		}
	}
}

static void
rw_dump_pipeline_air(MTLRenderPipelineDescriptor *descriptor)
{
	if (getenv("REDWOOD_DUMP_AIR") == NULL)
		return;
	rw_dump_air([descriptor vertexFunction]);
	rw_dump_air([descriptor fragmentFunction]);
}

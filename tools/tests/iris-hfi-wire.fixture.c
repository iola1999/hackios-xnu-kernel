#define ANNIBALE_IRIS_HOST_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#define PAGE_SIZE 16384u
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define DEVFS_CHAR 1
#define UID_ROOT 0
#define GID_WHEEL 0
#ifndef __APPLE__
typedef uintptr_t user_addr_t;
#endif
struct proc { int pid; };
struct cdevsw {
	int (*d_open)(dev_t, int, int, struct proc *);
	int (*d_close)(dev_t, int, int, struct proc *);
	void (*d_read)(void), (*d_write)(void);
	int (*d_ioctl)(dev_t, u_long, caddr_t, int, struct proc *);
	void (*d_stop)(void), (*d_reset)(void), (*d_select)(void), (*d_mmap)(void), (*d_strategy)(void);
	int d_type;
};
static void no_operation(void) {}
#define eno_rdwrt no_operation
#define eno_stop no_operation
#define eno_reset no_operation
#define eno_select no_operation
#define eno_mmap no_operation
#define eno_strat no_operation
#include "annibale_iris_private.h"
_Static_assert(sizeof(struct hfi_header) == 32, "native header");
_Static_assert(sizeof(struct hfi_packet) == 32, "native packet");
_Static_assert(sizeof(struct hfi_buffer) == 64, "native buffer");
static int lock_depth;
static void iris_lock_acquire(void) { assert(lock_depth++ == 0); }
static void iris_lock_release(void) { assert(--lock_depth == 0); }
static int iris_caller_pid(struct proc *p) { return p ? p->pid : 0; }
static int cdevsw_add(int n, const struct cdevsw *ops) { (void)n; (void)ops; return 42; }
static void *devfs_make_node(dev_t d, int t, int u, int g, int m, const char *n)
{ (void)d; (void)t; (void)u; (void)g; (void)m; (void)n; return (void *)1; }
#ifndef __APPLE__
static size_t strlcpy(char *d, const char *s, size_t n)
{ size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
#endif
static unsigned sleeps, alloc_calls, free_calls, free_attempts, copyin_calls, copyout_calls;
static unsigned alloc_fail_at, free_fail_at, block_after, stop_port, stop_mode, close_mode;
static bool alloc_retained, free_fail, copyout_fail;
static uint32_t tx_cursor;
static uint8_t tx[131072];
static size_t tx_size;
static unsigned tx_messages;
static const char *psc_path;
static bool automatic_psc, automatic_input, automatic_raw, raw_emitted, drain_auto_ack;

static uint32_t get32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(void *p, uint64_t v) { memcpy(p, &v, 8); }
static uint8_t *read_blob(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb"); assert(f && fseek(f, 0, SEEK_END) == 0);
	long n = ftell(f); assert(n >= 0 && n < 131072 && fseek(f, 0, SEEK_SET) == 0);
	uint8_t *data = calloc(1, (size_t)n + 4); assert(data);
	assert(fread(data, 1, (size_t)n, f) == (size_t)n && fclose(f) == 0);
	*size = (size_t)n; return data;
}
static void push_message(const uint8_t *data, size_t len)
{
	assert(len % 4 == 0 && len <= 2048);
	struct hfi_queue_header *q = (void *)(g_iris.uc_region.kva + 0x158u);
	uint32_t *ring = (void *)(g_iris.uc_region.kva + 0xc81c8u);
	for (size_t i = 0; i < len / 4; i++) memcpy(&ring[(q->write_idx + i) % 0x32000u], data + 4 * i, 4);
	q->write_idx = (q->write_idx + len / 4) % 0x32000u;
}
static void reply(uint32_t sid, uint32_t type, uint32_t id, uint32_t port, unsigned mode)
{
	if (mode == 6) return;
	uint8_t msg[96] = {0};
	put32(msg, mode == 7 ? 96 : 64); put32(msg + 4, mode == 1 ? sid + 99 : sid);
	put32(msg + 28, mode == 7 ? 2 : 1); put32(msg + 32, 32);
	put32(msg + 36, mode == 2 ? 0x01000005u : type);
	put32(msg + 40, mode == 5 ? 0 : 1);
	put32(msg + 48, mode == 3 ? port + 10 : port);
	put32(msg + 52, mode == 4 ? id + 1 : id);
	if (mode == 7) { put32(msg + 64, 32); put32(msg + 68, 0x05000002u); put32(msg + 72, 8); }
	push_message(msg, mode == 7 ? 96 : 64);
}
static void emit_buffer(uint32_t sid, uint32_t port, uint32_t type, uint32_t index,
    uint64_t address, uint32_t size, uint32_t data_size, uint64_t timestamp, uint32_t flags)
{
	uint8_t msg[128] = {0};
	put32(msg, 128); put32(msg + 4, sid); put32(msg + 28, 1);
	put32(msg + 32, 96); put32(msg + 36, 0x01000009u); put32(msg + 40, 1);
	put32(msg + 44, 5); put32(msg + 48, port); put32(msg + 64, type);
	put32(msg + 68, index); put64(msg + 72, address); put32(msg + 84, size);
	put32(msg + 92, data_size); put64(msg + 96, timestamp); put32(msg + 104, flags);
	push_message(msg, sizeof(msg));
}
static void push_psc(uint32_t sid)
{
	size_t n; uint8_t *data = read_blob(psc_path, &n); put32(data + 4, sid);
	push_message(data, n); free(data);
}
static void firmware_packet(uint32_t sid, const uint8_t *p)
{
	uint32_t type = get32(p + 4), port = get32(p + 16), id = get32(p + 20);
	if (type == 0x01000006u) reply(sid, type, id, port, port == stop_port ? stop_mode : 0);
	if (type == 0x01000004u) reply(sid, type, id, port, close_mode);
	if (type == 0x01000007u && drain_auto_ack) reply(sid, type, id, port, 0);
#ifdef IRIS_LEGACY_WIRE
	if (type == 0x01000003u || type == 0x01000005u) reply(sid, type, id, port, 0);
#endif
	if (type != 0x01000009u || get32(p + 12) != 5) return;
	const uint8_t *b = p + 32;
	if (get32(b) == 1) {
		if (automatic_psc) { automatic_psc = false; push_psc(sid); }
		if (automatic_input) emit_buffer(sid, 1, 1, get32(b + 4), get32(b + 8), get32(b + 20), 0, 0, 0);
	}
	if (get32(b) == 2 && automatic_raw && !raw_emitted) {
		raw_emitted = true;
		emit_buffer(sid, 2, 2, get32(b + 4), get32(b + 8), get32(b + 20), get32(b + 20), 0x2345, 0);
	}
}
void iris_cache_clean_inval(const void *address, size_t length)
{
	(void)length;
	if (!g_iris.uc_region.kva || address != (void *)(g_iris.uc_region.kva + 0x120u)) return;
	struct hfi_queue_header *q = (void *)address;
	uint32_t *ring = (void *)(g_iris.uc_region.kva + 0x1c8u);
	while (tx_cursor != q->write_idx) {
		uint32_t len = ring[tx_cursor]; assert(len >= 32 && len <= 1024 && len % 4 == 0);
		uint8_t msg[1024];
		for (uint32_t i = 0; i < len / 4; i++) memcpy(msg + 4 * i, &ring[(tx_cursor + i) % 0x32000u], 4);
		assert(tx_size + len <= sizeof(tx)); memcpy(tx + tx_size, msg, len); tx_size += len;
		tx_cursor = (tx_cursor + len / 4) % 0x32000u; tx_messages++;
		for (uint32_t off = 32; off < len; off += get32(msg + off)) firmware_packet(get32(msg + 4), msg + off);
	}
	q->read_idx = q->write_idx;
	if (block_after && tx_messages >= block_after) q->read_idx = (q->write_idx + 1) % 0x32000u;
}
void iris_sleep_us(uint32_t us) { assert(us == 250); sleeps++; }
bool iris_hw_probe_firmware(void) { return true; }
bool iris_hw_ensure_ready(void) { return g_iris.hw_ready; }
bool iris_dma_alloc_map(struct iris_dma_buf *buf, uint32_t iova, uint32_t size, bool cached)
{
	(void)cached; alloc_calls++; size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (alloc_calls == alloc_fail_at && !alloc_retained) return false;
	assert(size > 0 && size <= IRIS_IOVA_RAW_SESSION_STRIDE);
	buf->kva = (uintptr_t)calloc(1, size); assert(buf->kva); buf->iova = iova; buf->size = size;
	return alloc_calls != alloc_fail_at;
}
static bool release_buffer(struct iris_dma_buf *buf)
{
	if (!buf->kva) return true;
	free_attempts++;
	if (free_fail || free_attempts == free_fail_at) return false;
	free_calls++; free((void *)buf->kva); memset(buf, 0, sizeof(*buf)); return true;
}
#ifdef IRIS_LEGACY_WIRE
void iris_dma_free_unmap(struct iris_dma_buf *buf) { (void)release_buffer(buf); }
#else
bool iris_dma_free_unmap(struct iris_dma_buf *buf) { return release_buffer(buf); }
#endif
static int copyin(user_addr_t src, void *dst, size_t n) { copyin_calls++; memcpy(dst, (void *)src, n); return 0; }
static int copyout(const void *src, user_addr_t dst, size_t n)
{ copyout_calls++; if (copyout_fail) return EFAULT; memcpy((void *)dst, src, n); return 0; }

__HFI_SOURCE__
__SESSION_SOURCE__

static struct iris_session *prepare(uint32_t codec)
{
	g_iris.hw_ready = true;
	g_iris.uc_region.kva = (uintptr_t)calloc(1, 0x300000u);
	g_iris.sfr_region.kva = (uintptr_t)calloc(1, 4096u);
	g_iris.mmio = (uintptr_t)calloc(1, 0xf0000u);
	g_iris.smmu = (uintptr_t)calloc(1, 0x100000u);
	g_iris.next_header_id = 1000;
	g_iris.next_packet_id = 2000;
#ifndef IRIS_LEGACY_WIRE
	g_iris.next_wire_session_id = 100;
#endif
	struct iris_session *s = &g_iris.sessions[0];
	s->session_id = 1; s->codec_type = codec; s->width = 640; s->height = 480;
	s->active = true; s->owner_pid = 101; return s;
}
static unsigned count_type(uint32_t type, uint32_t port)
{
	unsigned count = 0;
	for (size_t pos = 0; pos < tx_size; pos += get32(tx + pos)) {
		for (uint32_t off = 32; off < get32(tx + pos); off += get32(tx + pos + off)) {
			const uint8_t *p = tx + pos + off;
			if (get32(p + 4) == type && get32(p + 16) == port) count++;
		}
	}
	return count;
}
static void wire_check(struct iris_session *s, const char *expected)
{
	assert(iris_hfi_session_open(s) == 0);
	size_t size; uint8_t *blob = read_blob(expected, &size);
	assert(tx_size == size && memcmp(tx, blob, size) == 0); free(blob);
	assert(count_type(0x01000005u, 1) == 1 && count_type(0x01000005u, 2) == 0);
	assert(count_type(0x01000009u, 2) == 0);
}
#ifndef IRIS_LEGACY_WIRE
static void open_output(struct iris_session *s)
{
	assert(iris_hfi_session_open(s) == 0); push_psc(s->wire_session_id);
	assert(iris_receive(s, NULL) == 1 && s->error == 0 && s->started_ports == 6);
}
static void output_wire_check(struct iris_session *s, const char *expected)
{
	open_output(s);
	size_t size; uint8_t *blob = read_blob(expected, &size);
	assert(tx_size == size && memcmp(tx, blob, size) == 0); free(blob);
	assert(count_type(0x01000009u, 2) == 6);
}
static void return_check(struct iris_session *s, const char *name)
{
	open_output(s); s->input_queued = true; s->next_input_idx = 77;
	bool raw = strstr(name, "raw") != NULL;
	uint32_t sid = s->wire_session_id, type = raw ? 2 : 1, port = raw ? 2 : 1;
	uint32_t index = raw ? 0 : 76, size = raw ? s->raw[0].size : s->bitstream.size;
	uint64_t base = raw ? s->raw[0].iova : s->bitstream.iova;
	uint32_t data = raw ? size : 0, flags = 0;
	if (strstr(name, "wrong-sid")) sid++;
	if (strstr(name, "index")) index += 5;
	if (strstr(name, "base")) base += 4096;
	if (strstr(name, "port")) port += 5;
	if (strstr(name, "size")) size--;
	if (strstr(name, "overflow")) data = size + 1;
	if (strstr(name, "short")) data = 1;
	if (strstr(name, "readonly")) flags = 16;
	if (strstr(name, "psc-last")) flags = 0x20000000u;
	emit_buffer(sid, port, type, index, base, size, data, 0x1234, flags);
	int r = iris_receive(s, NULL);
	if (strstr(name, "valid")) {
		assert(r == 1 && s->error == 0);
		assert(raw ? s->raw_state[0] == 2 : !s->input_queued);
	} else {
		assert(strstr(name, "wrong-sid") ? r == 1 : r < 0);
		assert(raw ? s->raw_state[0] == 1 : s->input_queued);
	}
	assert(free_calls == 0);
}
static void close_check(struct iris_session *s, const char *name)
{
	open_output(s);
	if (strcmp(name, "close-queue-failure") == 0) {
		block_after = tx_messages;
		iris_qhdr(0)->read_idx = (iris_qhdr(0)->write_idx + 1u) % IRIS_HFI_Q_DWORDS;
	}
	if (strcmp(name, "close-retry-stop") == 0) { stop_port = 2; stop_mode = 6; }
	if (strstr(name, "stop-")) { stop_port = strstr(name, "raw") ? 2 : 1; stop_mode = (unsigned)strtoul(strrchr(name, '-') + 1, NULL, 10); }
	if (strstr(name, "close-")) close_mode = (unsigned)strtoul(strrchr(name, '-') + 1, NULL, 10);
	if (strcmp(name, "tlb-failure") == 0) free_fail = true;
	int error = iris_hfi_session_close(s);
	if (strcmp(name, "close-success") == 0) {
		assert(error == 0 && free_calls == 11 && !s->opened && s->started_ports == 0);
		assert(count_type(0x01000006u, 1) == 1 && count_type(0x01000006u, 2) == 1);
		assert(count_type(0x01000004u, 0) == 1);
	} else {
		assert(error != 0 && free_calls == 0 && s->active && s->bitstream.kva);
		if (strcmp(name, "tlb-failure") == 0) assert(!s->opened);
		if (stop_port == 1 && stop_mode) assert(count_type(0x01000006u, 2) == 0);
		if (stop_mode) assert(count_type(0x01000004u, 0) == 0);
	}
	if (strcmp(name, "close-retry-stop") == 0) {
		assert(s->started_ports == 4 && !s->error && s->opened);
		stop_mode = 0;
		assert(iris_hfi_session_close(s) == 0 && free_calls == 11 && !s->opened);
		assert(count_type(0x01000006u, 1) == 1 && count_type(0x01000006u, 2) == 2);
	}
}
static void caller_check(struct iris_session *s, const char *name)
{
	memset(s, 0, sizeof(*s)); struct proc p = {101};
	struct iris_session_params sp = { .codec_type = 1, .width = 640, .height = 480 };
	if (strncmp(name, "alloc-", 6) == 0) {
		bool later = strcmp(name, "alloc-retained-later") == 0;
		alloc_fail_at = later ? 8 : 1;
		alloc_retained = strcmp(name, "alloc-clean") != 0;
		free_fail = alloc_retained && !later;
		free_fail_at = later ? 8 : 0;
		int error = iris_dev_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p);
		assert(error != 0 && sp.session_id == 0 && tx_size == 0);
		if (later) {
			assert(s->active && s->raw[0].kva && !s->bitstream.kva && !s->intern_bin.kva && free_calls == 7);
			free_fail_at = 0; uint32_t sid = 1;
			assert(iris_dev_ioctl(0, IRIS_IOC_CLOSE_SESSION, (caddr_t)&sid, 0, &p) == 0);
			assert(!s->active && free_calls == 8);
		} else assert(alloc_retained ? s->active && s->intern_bin.kva && free_calls == 0 : !s->active);
		return;
	}
	if (strcmp(name, "open-caller-retained") == 0) {
		block_after = 1;
		assert(iris_dev_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p) != 0);
		assert(s->active && s->opened && s->bitstream.kva && free_calls == 0); return;
	}
	assert(iris_dev_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&sp, 0, &p) == 0);
	close_mode = 6;
	if (strcmp(name, "close-caller-retained") == 0)
		assert(iris_dev_ioctl(0, IRIS_IOC_CLOSE_SESSION, (caddr_t)&sp.session_id, 0, &p) == ETIMEDOUT);
	else assert(iris_dev_close(0, 0, 0, &p) == ETIMEDOUT);
	assert(s->active && s->owner_pid == 101 && s->bitstream.kva && free_calls == 0);
	struct iris_session_params next = { .codec_type = 1, .width = 640, .height = 480 };
	assert(iris_dev_ioctl(0, IRIS_IOC_OPEN_SESSION, (caddr_t)&next, 0, &p) == 0 && next.session_id == 2);
}
static void drain_check(struct iris_session *s, const char *name)
{
	if (strncmp(name, "drain-empty-", 12) == 0) {
		assert(iris_hfi_session_open(s) == 0);
		size_t before = tx_size;
		if (strstr(name, "error")) s->error = EIO;
		if (strstr(name, "closing")) s->closing = true;
		int expected = s->error ? EIO : s->closing ? EBUSY : 0;
		assert(iris_hfi_session_flush(s) == expected);
		assert(tx_size == before && !s->draining && free_calls == 0);
		if (expected == 0) {
			/* An empty flush must leave the first compressed input publishable. */
			automatic_psc = automatic_input = automatic_raw = true;
			uint8_t input[64] = {0};
			struct iris_decode_frame r = { .input_user_addr = (uintptr_t)input, .input_len = 64, .in_tag = 0x2345 };
			assert(iris_hfi_decode_frame(s, &r) == 0 && s->next_input_idx == 1);
		}
		return;
	}
	open_output(s); s->input_queued = true; s->next_input_idx = 1;
	if (strcmp(name, "drain-flush-submitted") == 0) {
		assert(iris_hfi_session_flush(s) == ETIMEDOUT && s->draining);
		assert(count_type(0x01000007u, 1) == 1 && free_calls == 0);
		return;
	}
	assert(iris_begin_drain(s) == 0);
	reply(s->wire_session_id, 0x01000007u, s->drain_packet_id, 1, strstr(name, "wrong-id") ? 4 : 0);
	if (strstr(name, "last") || strstr(name, "complete")) reply(s->wire_session_id, 0x06000006u, 0, 0, 0);
	if (!strstr(name, "input-held")) emit_buffer(s->wire_session_id, 1, 1, 0, s->bitstream.iova, s->bitstream.size, 0, 0, 0);
	int error = iris_collect_output(s, NULL, true, false);
	if (strcmp(name, "drain-complete") == 0) {
		assert(error == 0 && !s->draining);
		assert(count_type(0x01000008u, 1) == 2 && count_type(0x01000008u, 2) == 1);
	} else {
		assert(error == ETIMEDOUT && s->draining);
		assert(count_type(0x01000008u, 1) == 1 && count_type(0x01000008u, 2) == 0);
		assert(iris_finish_drain(s) == EBUSY);
	}
	assert(free_calls == 0);
}
static void output_copy_check(struct iris_session *s)
{
	open_output(s);
	emit_buffer(s->wire_session_id, 2, 2, 0, s->raw[0].iova, s->raw[0].size, s->raw[0].size, 0x2345, 0);
	assert(iris_receive(s, NULL) == 1 && s->raw_state[0] == 2);
	uint8_t *y = calloc(1, s->y_stride * s->height), *uv = calloc(1, s->uv_stride * s->height / 2);
	struct iris_decode_frame req = { .out_user_y = (uintptr_t)y, .out_user_uv = (uintptr_t)uv };
	size_t before = tx_size; copyout_fail = true;
	assert(iris_take_output(s, &req) == -EFAULT && s->raw_state[0] == 2 && tx_size == before);
	copyout_fail = false; copyout_calls = 0;
	assert(iris_take_output(s, &req) == 1 && s->raw_state[0] == 1 && tx_size > before);
	assert(req.out_tag == 0x2345 && req.flags == 1 && copyout_calls == s->height * 3 / 2);
	free(y); free(uv);
}
static void later_error_check(struct iris_session *s, const char *name)
{
	assert(iris_hfi_session_open(s) == 0); s->input_queued = true; s->next_input_idx = 1;
	bool property = strstr(name, "property") != NULL, system = strstr(name, "system") != NULL;
	uint8_t msg[164] = {0}; uint32_t length = property ? 164 : 160;
	put32(msg, length); put32(msg + 4, s->wire_session_id); put32(msg + 28, 2);
	put32(msg + 32, 96); put32(msg + 36, 0x01000009u); put32(msg + 40, 1);
	put32(msg + 44, 5); put32(msg + 48, 1); put32(msg + 64, 1);
	put64(msg + 72, s->bitstream.iova); put32(msg + 84, s->bitstream.size);
	put32(msg + 128, property ? 36 : 32);
	put32(msg + 132, property ? 0x03000103u : system ? 0x05000002u : 0x04000005u);
	put32(msg + 136, property ? 1 : system ? 8 : 4);
	if (property) { put32(msg + 140, 9); put32(msg + 144, 1); put32(msg + 160, 640u << 16 | 480u); }
	push_message(msg, length);
	assert(iris_receive(s, NULL) == -EIO && s->input_queued && s->error == EIO && free_calls == 0);
	assert(g_iris.hw_ready != system);
}
#endif

static unsigned buffer_count(uint32_t type)
{
    unsigned count = 0;
    for (size_t pos = 0; pos < tx_size; pos += get32(tx + pos)) {
        for (uint32_t off = 32; off < get32(tx + pos); off += get32(tx + pos + off)) {
            const uint8_t *p = tx + pos + off;
            if (get32(p + 4) == 0x01000009u && get32(p + 16) == 2 && get32(p + 32) == type) count++;
        }
    }
    return count;
}

static void raw_count_check(struct iris_session *s, const char *name)
{
    unsigned minimum = 6;
    const char *number = strrchr(name, '-');
    if (strstr(name, "negotiated")) minimum = (unsigned)strtoul(number + 1, NULL, 10);
    if (strstr(name, "unallocated")) minimum = 1;
    if (strstr(name, "capacity-isolated")) minimum = 7;
    if (strstr(name, "slot4")) s->session_id = 4;
    if (strstr(name, "cd21-actual")) { s->width = 320; s->height = 192; }
    assert(iris_hfi_session_open(s) == 0);
    assert(s->raw_count == 4 && alloc_calls == 11);
    uintptr_t initial[4];
    for (unsigned i = 0; i < 4; i++) initial[i] = s->raw[i].kva;
    if (strstr(name, "range-exact")) s->raw_frame_size = IRIS_IOVA_RAW_SESSION_STRIDE / 6;
    if (strstr(name, "range-over")) s->raw_frame_size = IRIS_IOVA_RAW_SESSION_STRIDE / 6 + PAGE_SIZE;
    if (strstr(name, "range-wrap")) s->raw_frame_size = 0x80000000u;
    if (strstr(name, "zero-frame")) s->raw_frame_size = 0;
    if (strstr(name, "dpb-short")) s->intern_dpb.size = 5 * s->dpb_frame_size;
    if (strstr(name, "capacity-isolated")) s->intern_dpb.size = 8 * s->dpb_frame_size;
    if (strstr(name, "closing")) s->closing = true;
    bool failing_alloc = strstr(name, "alloc-fail") != NULL;
    unsigned fail_index = strstr(name, "second") ? 5 : 4;
    if (failing_alloc) {
        alloc_fail_at = 8 + fail_index;
        alloc_retained = strstr(name, "retained") != NULL;
    }
    s->input_queued = true; s->next_input_idx = 1;
    size_t before = tx_size;
    push_psc(s->wire_session_id);
    int result = iris_receive(s, NULL);
    bool invalid = minimum == 0 || minimum > 6 || strstr(name, "range-over") ||
        strstr(name, "range-wrap") || strstr(name, "zero-frame") || strstr(name, "dpb-short") || strstr(name, "closing");
    if (invalid || failing_alloc) {
        assert(result < 0 && s->error == (failing_alloc ? ENOMEM : EOPNOTSUPP));
        assert(s->started_ports == 2 && tx_size == before && free_calls == 0 && s->input_queued);
        assert(s->raw_count == (failing_alloc ? fail_index : 4));
        if (!failing_alloc) assert(alloc_calls == 11);
        for (unsigned i = 0; i < 4; i++) assert(s->raw[i].kva == initial[i]);
        if (failing_alloc) {
            assert(alloc_calls == fail_index + 8);
            assert((s->raw[fail_index].kva != 0) == alloc_retained);
            assert(iris_hfi_session_close(s) == ENOMEM && free_calls == 0);
            /* Isolate cleanup of host allocations after checking the live-session retention path. */
            unsigned allocated = 7 + fail_index + (alloc_retained ? 1 : 0);
            if (alloc_retained) {
                free_fail_at = 8 + fail_index;
                assert(!iris_free_session_buffers(s) && s->raw[fail_index].kva != 0);
                free_fail_at = 0;
            }
            assert(iris_free_session_buffers(s) && free_calls == allocated);
            for (unsigned i = 0; i < IRIS_RAW_POOL_MAX; i++) assert(!s->raw[i].kva);
        }
        return;
    }
    unsigned actual = minimum < 4 ? 4 : minimum;
    assert(result == 1 && !s->error && s->started_ports == 6 && s->dpb_count == minimum);
    assert(s->raw_count == actual && alloc_calls == 7 + actual && free_calls == 0);
    assert(buffer_count(2) == actual && buffer_count(6) == minimum);
    for (unsigned i = 0; i < actual; i++) {
        if (i < 4) assert(s->raw[i].kva == initial[i]);
        else assert(s->raw[i].iova == 0x60000000u + (s->session_id - 1u) * 0x03000000u + i * s->raw_frame_size);
        assert(s->raw[i].kva && s->raw_state[i] == 1);
        assert((uint64_t)s->raw[i].iova + s->raw[i].size <= 0x60000000ull + s->session_id * 0x03000000ull);
    }
    if (strstr(name, "return-last")) {
        unsigned last = actual - 1;
        emit_buffer(s->wire_session_id, 2, 2, last, s->raw[last].iova, s->raw[last].size,
            s->raw[last].size, 0x9876, 0);
        assert(iris_receive(s, NULL) == 1 && s->raw_state[last] == 2);
        struct iris_decode_frame req = {0};
        assert(iris_take_output(s, &req) == 1 && req.out_tag == 0x9876 && req.flags == 1);
        assert(s->raw_state[last] == 1 && buffer_count(2) == actual + 1);
    }
    if (strstr(name, "drain-last")) {
        s->raw_state[actual - 1] = IRIS_RAW_HOST;
        s->draining = s->drain_ack = s->drain_last = true; s->input_queued = false;
        assert(iris_finish_drain(s) == 0 && s->raw_state[actual - 1] == 1);
        assert(buffer_count(2) == actual + 1);
    }
    if (strstr(name, "unallocated")) {
        s->raw_state[4] = IRIS_RAW_QUEUED;
        emit_buffer(s->wire_session_id, 2, 2, 4, 0, 0, 0, 0, 0);
        assert(iris_receive(s, NULL) == -EIO && s->raw_state[4] == IRIS_RAW_QUEUED && free_calls == 0);
        return;
    }
    if (strstr(name, "repeat") || strstr(name, "changing")) {
        before = tx_size; unsigned allocations = alloc_calls;
        if (strstr(name, "changing")) {
            size_t n; uint8_t *blob = read_blob(psc_path, &n); put32(blob + 4, s->wire_session_id);
            for (uint32_t off = 32; off < n; off += get32(blob + off))
                if (get32(blob + off + 4) == 0x03000124u) put32(blob + off + 32, 5);
            push_message(blob, n); free(blob);
        } else push_psc(s->wire_session_id);
        assert(iris_receive(s, NULL) == -EOPNOTSUPP && tx_size == before && alloc_calls == allocations);
        assert(s->raw_count == actual && free_calls == 0);
        return;
    }
    if (strstr(name, "stop-fail")) {
        stop_port = strstr(name, "input") ? 1 : 2; stop_mode = 6;
        assert(iris_hfi_session_close(s) == ETIMEDOUT && free_calls == 0);
        for (unsigned i = 0; i < actual; i++) assert(s->raw[i].kva != 0);
        stop_mode = 0;
    }
    s->input_queued = false;
    assert(iris_hfi_session_close(s) == 0 && free_calls == 7 + actual);
    for (unsigned i = 0; i < IRIS_RAW_POOL_MAX; i++) assert(!s->raw[i].kva);
}

static void stage_check(struct iris_session *s, const char *name)
{
    unsigned expected;
    if (strcmp(name, "stage-wire") != 0) {
        assert(sscanf(name, "stage-%u-%u-%u", &s->width, &s->height, &expected) == 3);
        assert(iris_decode_stage(s) == expected);
        return;
    }
    s->width = 1280; s->height = 64;
    assert(iris_hfi_session_open(s) == 0);
    unsigned stages = 0;
    for (size_t pos = 0; pos < tx_size; pos += get32(tx + pos)) {
        size_t end = pos + get32(tx + pos);
        for (size_t off = pos + 32; off < end; off += get32(tx + off)) {
            if (get32(tx + off + 4) != 0x0300010a) continue;
            assert(get32(tx + off) == 36 && get32(tx + off + 8) == 0);
            assert(get32(tx + off + 12) == 1 && get32(tx + off + 16) == 0);
            assert(get32(tx + off + 32) == 2); stages++;
        }
    }
    assert(stages == 1);
    assert(iris_hfi_session_close(s) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 4); const char *name = argv[1]; psc_path = argv[2];
	struct iris_session *s = prepare(strstr(name, "hevc") ? 2 : 1);
	if (strncmp(name, "stage-", 6) == 0) stage_check(s, name);
	else if (strncmp(name, "raw-", 4) == 0) raw_count_check(s, name);
	else if (strncmp(name, "wire-", 5) == 0) {
#ifndef IRIS_LEGACY_WIRE
		if (strncmp(name, "wire-psc-", 9) == 0) output_wire_check(s, argv[3]); else
#endif
		wire_check(s, argv[3]);
	}
#ifndef IRIS_LEGACY_WIRE
	else if (strncmp(name, "buffer-", 7) == 0) return_check(s, name);
	else if (strncmp(name, "stop-", 5) == 0 || strncmp(name, "close-", 6) == 0 || strcmp(name, "tlb-failure") == 0) {
		if (strstr(name, "caller")) caller_check(s, name); else close_check(s, name);
	}
	else if (strstr(name, "caller") || strncmp(name, "alloc-", 6) == 0) caller_check(s, name);
	else if (strncmp(name, "drain-", 6) == 0) drain_check(s, name);
	else if (strcmp(name, "output-copy") == 0) output_copy_check(s);
	else if (strncmp(name, "later-", 6) == 0) later_error_check(s, name);
	else if (strcmp(name, "input-immutable") == 0) {
		assert(iris_hfi_session_open(s) == 0); memset((void *)s->bitstream.kva, 0xa5, 64);
		s->input_queued = true;
		uint8_t input[64] = {0}; struct iris_decode_frame r = { .input_user_addr = (uintptr_t)input, .input_len = 64 };
		assert(iris_hfi_decode_frame(s, &r) == EBUSY && copyin_calls == 0 && *(uint8_t *)s->bitstream.kva == 0xa5);
	}
	else if (strcmp(name, "decode-psc") == 0) {
		assert(iris_hfi_session_open(s) == 0); automatic_psc = automatic_input = automatic_raw = true;
		uint8_t input[64] = {0}; struct iris_decode_frame r = { .input_user_addr = (uintptr_t)input, .input_len = 64, .in_tag = 0x2345 };
		assert(iris_hfi_decode_frame(s, &r) == 0 && r.flags == 1 && r.out_tag == 0x2345);
		assert(!s->input_queued && s->started_ports == 6 && s->raw_state[0] == 1);
	}
	else if (strncmp(name, "psc-", 4) == 0) {
		assert(iris_hfi_session_open(s) == 0); size_t before = tx_size;
		s->input_queued = true; s->next_input_idx = 1;
		push_psc(s->wire_session_id); int r = iris_receive(s, NULL);
		if (strncmp(name, "psc-valid", 9) == 0) {
			assert(r == 1 && s->error == 0 && s->started_ports == 6 && s->dpb_count == 2);
			assert(count_type(0x01000009u, 2) == 6);
		} else assert(r < 0 && s->error != 0 && s->started_ports == 2 && tx_size == before &&
		    s->input_queued && free_calls == 0);
	}
	else if (strcmp(name, "stale-wire") == 0) {
		assert(iris_hfi_session_open(s) == 0); uint32_t old = s->wire_session_id;
		assert(iris_hfi_session_close(s) == 0); memset(s, 0, sizeof(*s));
		s->active = true; s->session_id = 1; s->width = 640; s->height = 480; s->codec_type = 1;
		assert(iris_hfi_session_open(s) == 0 && s->wire_session_id > old);
		s->input_queued = true; s->next_input_idx = 1;
		emit_buffer(old, 1, 1, 0, s->bitstream.iova, s->bitstream.size, 0, 0, 0);
		assert(iris_receive(s, NULL) == 1 && s->input_queued && s->error == 0);
	}
	else if (strcmp(name, "cross-session") == 0) {
		assert(iris_hfi_session_open(s) == 0); struct iris_session *other = &g_iris.sessions[1];
		other->active = true; other->session_id = 2; other->width = 640; other->height = 480; other->codec_type = 1;
		assert(iris_hfi_session_open(other) == 0); other->input_queued = true; other->next_input_idx = 1;
		emit_buffer(other->wire_session_id, 1, 1, 0, other->bitstream.iova, other->bitstream.size, 0, 0, 0);
		assert(iris_receive(s, NULL) == 1 && !other->input_queued && s->error == 0);
	}
	else if (strcmp(name, "wait-cross-session") == 0) {
		assert(iris_hfi_session_open(s) == 0); struct iris_session *other = &g_iris.sessions[1];
		other->active = true; other->wire_session_id = s->wire_session_id + 1;
		struct iris_reply_wait wait = { s->wire_session_id, 0x01000006u, 999, 1, false };
		reply(other->wire_session_id, 0x01000006u, 999, 1, 0);
		assert(iris_receive(s, &wait) == 1 && !wait.done && !s->error && !other->error && free_calls == 0);
	}
	else if (strcmp(name, "truncated-later") == 0) {
		assert(iris_hfi_session_open(s) == 0); s->input_queued = true; s->next_input_idx = 1;
		uint8_t msg[64] = {0}; put32(msg, 64); put32(msg + 4, s->wire_session_id); put32(msg + 28, 1);
		put32(msg + 32, 96); put32(msg + 36, 0x01000009u); put32(msg + 44, 5); push_message(msg, 64);
		assert(iris_receive(s, NULL) == -EIO && s->input_queued && free_calls == 0 && !g_iris.hw_ready);
	}
	else if (strcmp(name, "payload-none") == 0) {
		assert(iris_hfi_session_open(s) == 0); s->input_queued = true;
		reply(s->wire_session_id, 0x01000009u, 0, 1, 0);
		assert(iris_receive(s, NULL) == 1 && s->input_queued && s->error == 0);
	}
	else if (strcmp(name, "range-reject") == 0) {
		s->width = s->height = 4096;
		assert(iris_hfi_session_open(s) == EOPNOTSUPP && alloc_calls == 0 && tx_size == 0);
	}
	else if (strncmp(name, "sizes-", 6) == 0) {
		size_t n; uint8_t *blob = read_blob(argv[3], &n); assert(n == 48);
		s->width = get32(blob); s->height = get32(blob + 4); s->codec_type = get32(blob + 8);
		if (!get32(blob + 12)) alloc_fail_at = 1;
		int error = iris_alloc_session_buffers(s);
		if (get32(blob + 12)) {
			assert(error == 0 && s->dpb_frame_size == get32(blob + 40) && s->raw_frame_size == get32(blob + 44));
			struct iris_dma_buf *bufs[] = { &s->intern_bin, &s->intern_comv, &s->intern_non_comv,
				&s->intern_line, &s->intern_persist, &s->intern_dpb };
			for (unsigned i = 0; i < 6; i++) assert(bufs[i]->size == get32(blob + 16 + 4 * i));
		} else assert(error == EOPNOTSUPP && alloc_calls == 0);
		free(blob);
	}
	else assert(!"unknown case");
#endif
	printf("PASS: %s actual HFI/session functions\n", name); return 0;
}

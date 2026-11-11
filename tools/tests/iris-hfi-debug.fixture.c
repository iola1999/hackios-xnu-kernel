#define ANNIBALE_IRIS_HOST_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#define PAGE_SIZE 16384u
#include "annibale_iris_private.h"
#undef iris_say
static void diagnostic_print(const char *format, ...) {
    char line[320]; va_list args;
    va_start(args, format); int length = vsnprintf(line, sizeof(line), format, args); va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(line)); fputs(line, stdout);
}
#define iris_say(...) diagnostic_print("iris: " __VA_ARGS__)
struct iris_controller g_iris;
static bool reuse_consumed, append_record;
static uint32_t last_read;
static unsigned cache_calls;
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
void iris_cache_clean_inval(const void *address, size_t size) {
    uintptr_t a = (uintptr_t)address;
    assert((a >= g_iris.uc_region.kva && a + size <= g_iris.uc_region.kva + g_iris.uc_region.size) ||
           (a >= g_iris.sfr_region.kva && a + size <= g_iris.sfr_region.kva + g_iris.sfr_region.size));
    assert(++cache_calls < 1000);
    struct hfi_queue_header *q = (void *)(g_iris.uc_region.kva + 0x190u);
    if (address == q && last_read != q->read_idx) {
        if (reuse_consumed) {
            uint32_t *ring = (void *)(g_iris.uc_region.kva + 0x1901c8u);
            while (last_read != q->read_idx) { ring[last_read] = 0xeeeeeeee; last_read = (last_read + 1) % 0x32000u; }
        }
        if (append_record) {
            uint32_t *ring = (void *)(g_iris.uc_region.kva + 0x1901c8u);
            uint8_t next[24] = {24, 0, 0, 0, 2}; memcpy(next + 17, "late", 5);
            for (unsigned i = 0; i < 6; i++) memcpy(&ring[(q->write_idx + i) % 0x32000u], next + 4 * i, 4);
            q->write_idx = (q->write_idx + 6) % 0x32000u;
        }
        last_read = q->read_idx;
    }
}
void iris_sleep_us(uint32_t us) { (void)us; }
bool iris_dma_alloc_map(struct iris_dma_buf *b, uint32_t i, uint32_t s, bool c)
{ (void)b; (void)i; (void)s; (void)c; assert(false); return false; }
bool iris_dma_free_unmap(struct iris_dma_buf *b) { (void)b; assert(false); return false; }
static int copyin(user_addr_t s, void *d, size_t n) { (void)s; (void)d; (void)n; assert(false); return EIO; }
static int copyout(const void *s, user_addr_t d, size_t n) { (void)s; (void)d; (void)n; assert(false); return EIO; }

__HFI_SOURCE__

int main(int argc, char **argv) {
    assert(argc == 8);
    const char *mode = argv[1];
    uint32_t start = (uint32_t)strtoul(argv[3], NULL, 0), published = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t expected = (uint32_t)strtoul(argv[5], NULL, 0);
    g_iris.uc_region.size = 0x1901c8u + 0xc8000u;
    g_iris.uc_region.kva = (uintptr_t)calloc(1, g_iris.uc_region.size);
    g_iris.sfr_region.size = (uint32_t)strtoul(argv[7], NULL, 0);
    g_iris.sfr_region.kva = (uintptr_t)calloc(1, g_iris.sfr_region.size);
    g_iris.mmio = (uintptr_t)calloc(1, 0xf0000u);
    assert(g_iris.uc_region.kva && g_iris.sfr_region.kva && g_iris.mmio);
    uint8_t data[81920]; FILE *f = fopen(argv[2], "rb"); assert(f);
    size_t size = fread(data, 1, sizeof(data), f); assert(feof(f) && fclose(f) == 0 && size % 4 == 0);
    uint32_t *ring = (void *)(g_iris.uc_region.kva + 0x1901c8u);
    for (size_t i = 0; i < size / 4; i++) memcpy(&ring[(start + i) % 0x32000u], data + i * 4, 4);
    struct hfi_queue_header *q = (void *)(g_iris.uc_region.kva + 0x190u);
    q->read_idx = start; q->write_idx = (start + published / 4) % 0x32000u;
    if (!strcmp(mode, "bad-read")) q->read_idx = 0x32000u;
    if (!strcmp(mode, "bad-write")) q->write_idx = 0x32000u;
    last_read = q->read_idx; reuse_consumed = !strcmp(mode, "reuse"); append_record = !strcmp(mode, "append");
    f = fopen(argv[6], "rb"); assert(f);
    size = fread((void *)g_iris.sfr_region.kva, 1, g_iris.sfr_region.size, f); assert(fclose(f) == 0);
    (void)size;
    g_iris.hw_ready = true;
    struct iris_session *s = &g_iris.sessions[0];
    s->active = true; s->wire_session_id = 77; s->input_queued = true; s->raw_state[0] = IRIS_RAW_QUEUED;
    if (!strcmp(mode, "session") || !strcmp(mode, "session-flag") || !strcmp(mode, "unknown")) {
        uint32_t msg[16] = {64, 77, 1, 0, 0, 0, 0, 1, 32, 0x04000001, 0, 0, 0, 1};
        if (!strcmp(mode, "session-flag")) { msg[9] = 0x03000003; msg[10] = 4; }
        if (!strcmp(mode, "unknown")) msg[1] = 99;
        int error = iris_dispatch_message((uint8_t *)msg, sizeof(msg), s, NULL);
        assert(error == (!strcmp(mode, "unknown") ? 0 : EIO));
        assert(s->error == error && g_iris.hw_ready && s->input_queued && s->raw_state[0] == IRIS_RAW_QUEUED);
    } else if (!strcmp(mode, "init")) {
        uint32_t msg[16] = {64, 0, 1, 0, 0, 0, 0, 1, 32, 0x04000001};
        memcpy((void *)(g_iris.uc_region.kva + 0xc81c8u), msg, sizeof(msg));
        ((struct hfi_queue_header *)(g_iris.uc_region.kva + 0x158u))->write_idx = 16;
        assert(!iris_hfi_sys_init());
    } else iris_drain_dbgq();
    assert(q->read_idx == expected);
    printf("PASS read=%u write=%u cache=%u\n", q->read_idx, q->write_idx, cache_calls);
    free((void *)g_iris.uc_region.kva); free((void *)g_iris.sfr_region.kva); free((void *)g_iris.mmio);
    return 0;
}

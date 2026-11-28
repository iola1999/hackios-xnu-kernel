#!/usr/bin/env python3
"""Host test of the dual-instance TFTP service (annibale_tftp.inc) compiled with stub QRTR."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "xnu/files/bsd/dev/annibale/annibale_tftp.inc").read_text()

HARNESS = r'''
#define ANNIBALE_TFTP_HOST_TEST
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define Z_WAITOK 0
#define Z_ZERO 1
static void *kalloc_data(size_t size, int flags) { return (flags & Z_ZERO) ? calloc(1, size) : malloc(size); }
static void kfree_data(void *p, size_t size) { (void)size; free(p); }
static int tftp_lock, tftp_init_lock;
static void lck_mtx_lock(int *l) { assert(l == &tftp_lock || l == &tftp_init_lock); }
static void lck_mtx_unlock(int *l) { assert(l == &tftp_lock || l == &tftp_init_lock); }
static size_t strlcpy_host(char *dst, const char *src, size_t sz) {
    size_t len = strlen(src);
    if (sz != 0) {
        size_t copy = len < sz - 1 ? len : sz - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#undef strlcpy
#define strlcpy strlcpy_host
struct annibale_qrtr_port { int unused; };
typedef void (*annibale_qrtr_receive_t)(void *, uint32_t, uint32_t, const void *, size_t);
static struct annibale_qrtr_port the_port;
static uint8_t sent[64][8192];
static size_t sent_size[64];
static unsigned sent_count, published_count;
static uint32_t published_services[8], published_instances[8];
/* paced: a block over 1000 bytes needs the one big intent, which a poll of the link gives back. */
static bool paced, big_intent;
static unsigned link_polls, refused;
static void link_poll(void) { link_polls++; big_intent = true; }
static void tftp_sleep_ms(unsigned ms) { assert(ms == 1); }
static int stub_send(struct annibale_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length) {
    assert(port == &the_port && (node == 0 || node == 5u) && to_port != 0 && length <= 8192 - 32 && sent_count < 64);
    if (paced && length > 1000) {
        if (!big_intent) { refused++; return 35; }
        big_intent = false;
    }
    memcpy(sent[sent_count], data, length); sent_size[sent_count++] = length; return 0;
}
static struct annibale_qrtr_port *stub_open(annibale_qrtr_receive_t receive, void *server, void *context) {
    (void)receive; (void)server; (void)context; return &the_port;
}
static const struct { struct annibale_qrtr_port *(*open)(annibale_qrtr_receive_t, void *, void *);
    int (*send)(struct annibale_qrtr_port *, uint32_t, uint32_t, const void *, size_t); } annibale_qrtr = { stub_open, stub_send };
static int annibale_qrtr_publish(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance) {
    assert(port == &the_port && version == 1 && published_count < 8);
    published_services[published_count] = service;
    published_instances[published_count++] = instance;
    return 0;
}
typedef size_t vm_size_t;
static uint8_t volume_file[30000];
static unsigned volume_reads;
static bool annibale_ufs_fat16_path_read(const char *path, uint64_t *size_out, volatile void *dst, uint64_t cap) {
    if (strcmp(path, "image/modem_pr/mcfg/a.mbn") && strcmp(path, "image/b.mbn") && strcmp(path, "image/c.mbn") &&
        strcmp(path, "image/d.mbn") && strcmp(path, "image/e.mbn")) return false;
    *size_out = sizeof(volume_file);
    if (dst == NULL) return true;
    assert(cap >= sizeof(volume_file)); volume_reads++;
    memcpy((void *)dst, volume_file, sizeof(volume_file)); return true;
}
static const char k_sns_reg[] = "version=1\nsensor=gls6151\n";
static const char k_sns_list[] = "ambient_light\n";
static const char k_hlos_sns[] = "{\"sns\":\"reg\"}\n";
static bool tftp_image_read(const char *rel, uint8_t **data_out, uint32_t *size_out, uint32_t *room_out, bool *borrowed_out) {
    if (strcmp(rel, "sensors/sensors_list.txt") == 0) {
        *data_out = (uint8_t *)(uintptr_t)k_sns_list;
        *size_out = (uint32_t)(sizeof(k_sns_list) - 1u);
        *room_out = 0; *borrowed_out = true;
        return true;
    }
    if (strcmp(rel, "sensors/hwMisList.txt") == 0) {
        *data_out = (uint8_t *)(uintptr_t)"";
        *size_out = 0; *room_out = 0; *borrowed_out = true;
        return true;
    }
    if (strcmp(rel, "hlos_rfs/shared/sensors/sns_reg_config") == 0) {
        *data_out = (uint8_t *)(uintptr_t)k_hlos_sns;
        *size_out = (uint32_t)(sizeof(k_hlos_sns) - 1u);
        *room_out = 0; *borrowed_out = true;
        return true;
    }
    return false;
}
static bool tftp_vfs_read(const char *path, uint8_t **data_out, uint32_t *size_out, uint32_t *room_out, bool *borrowed_out) {
    if (strcmp(path, "/vendor/etc/sensors/sns_reg_config") == 0) {
        *data_out = (uint8_t *)(uintptr_t)strdup(k_sns_reg);
        *size_out = (uint32_t)(sizeof(k_sns_reg) - 1u);
        *room_out = *size_out + 1u;
        *borrowed_out = false;
        return true;
    }
    return false;
}
#define printf(...) ((void)0)
@SOURCE@
#undef printf

static size_t request(uint8_t *out, unsigned opcode, const char *name, const char *options[], unsigned pairs) {
    size_t n = 2; out[0] = 0; out[1] = (uint8_t)opcode;
    n += (size_t)snprintf((char *)out + n, 256, "%s", name) + 1;
    n += (size_t)snprintf((char *)out + n, 256, "octet") + 1;
    for (unsigned i = 0; i < pairs * 2; i++) n += (size_t)snprintf((char *)out + n, 256, "%s", options[i]) + 1;
    return n;
}
static void rx_node(uint32_t node, uint32_t port, const uint8_t *data, size_t size) { svc_tftp_rx(NULL, node, port, data, size); }
static void rx(uint32_t port, const uint8_t *data, size_t size) { rx_node(0, port, data, size); }
static void ack(uint32_t port, unsigned block) { uint8_t p[4] = { 0, 4, (uint8_t)(block >> 8), (uint8_t)block }; rx(port, p, 4); }
static void data_block(uint32_t port, unsigned block, const uint8_t *bytes, size_t n) {
    static uint8_t p[8192]; p[0] = 0; p[1] = 3; p[2] = (uint8_t)(block >> 8); p[3] = (uint8_t)block;
    memcpy(p + 4, bytes, n); rx(port, p, 4 + n);
}
static bool option_is(const uint8_t *oack, size_t size, const char *name, const char *value) {
    for (size_t at = 2; at < size;) {
        const char *n = (const char *)oack + at; at += strlen(n) + 1;
        const char *v = (const char *)oack + at; at += strlen(v) + 1;
        if (!strcmp(n, name)) return !strcmp(v, value);
    }
    return false;
}

int main(void) {
    uint8_t pkt[512];
    static uint8_t body[20000];
    for (unsigned i = 0; i < sizeof(body); i++) body[i] = (uint8_t)(i * 7 + 3);
    annibale_tftp_start(TFTP_INST_ADSP);
    annibale_tftp_start(TFTP_INST_ADSP); /* idempotent */
    annibale_tftp_start(TFTP_INST_MPSS);
    assert(published_count == 2u);
    assert(published_services[0] == 0x1000u && published_instances[0] == 2u);
    assert(published_services[1] == 0x1000u && published_instances[1] == 0u);

    /* What MPSS sends first: the server check, with its options. */
    const char *mpss[] = { "blksize", "15872", "timeoutms", "5000", "wsize", "10" };
    size_t n = request(pkt, 2, "/readwrite/server_check.txt", mpss, 3);
    rx(36, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 6);
    assert(option_is(sent[0], sent_size[0], "blksize", "7168") && option_is(sent[0], sent_size[0], "timeoutms", "5000"));
    assert(option_is(sent[0], sent_size[0], "wsize", "10") && !option_is(sent[0], sent_size[0], "tsize", "0"));
    data_block(36, 1, (const uint8_t *)"hello", 5);
    assert(sent_count == 2 && sent[1][1] == 4 && sent[1][3] == 1);
    /* ... and read back, without options: plain 512-byte blocks, one per acknowledgement. */
    sent_count = 0; n = request(pkt, 1, "/readwrite/server_check.txt", NULL, 0); rx(37, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent[0][3] == 1 && sent_size[0] == 9 && !memcmp(sent[0] + 4, "hello", 5));
    ack(37, 1); assert(sent_count == 1);

    /* A write of several windows: an acknowledgement each wsize blocks and at the short block; a repeat is not written twice. */
    const char *wide[] = { "blksize", "1000", "wsize", "3", "tsize", "20000" };
    sent_count = 0; n = request(pkt, 2, "/readwrite/big", wide, 3); rx(40, pkt, n);
    assert(sent_count == 1 && option_is(sent[0], sent_size[0], "blksize", "1000") && option_is(sent[0], sent_size[0], "tsize", "20000"));
    for (unsigned block = 1; block <= 20; block++) {
        sent_count = 0; data_block(40, block, body + (block - 1) * 1000, 1000);
        assert(sent_count == (block % 3 == 0 ? 1u : 0u));
        if (block == 5) { data_block(40, 5, body + 4000, 1000); assert(sent_count == 0); }
    }
    sent_count = 0; data_block(40, 21, body, 0);
    assert(sent_count == 1 && sent[0][1] == 4 && sent[0][3] == 21);
    assert(tftp_file_named("/readwrite/big", false)->size == 20000 && !memcmp(tftp_file_named("/readwrite/big", false)->data, body, 20000));

    /* A read with options: OACK with the size, then a window per acknowledgement, the short block last. */
    const char *reading[] = { "blksize", "4096", "wsize", "2", "tsize", "0" };
    sent_count = 0; n = request(pkt, 1, "/readwrite/big", reading, 3); rx(41, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 6 && option_is(sent[0], sent_size[0], "tsize", "20000"));
    sent_count = 0; ack(41, 0);
    assert(sent_count == 2 && sent_size[0] == 4100 && sent[1][3] == 2 && !memcmp(sent[1] + 4, body + 4096, 4096));
    sent_count = 0; ack(41, 2); assert(sent_count == 2);
    sent_count = 0; ack(41, 4);
    assert(sent_count == 1 && sent[0][3] == 5 && sent_size[0] == 4 + 20000 - 16384 && !memcmp(sent[0] + 4, body + 16384, 3616));
    sent_count = 0; ack(41, 5); assert(sent_count == 0);

    /* rsize and seek: that many bytes from that offset. */
    const char *part[] = { "blksize", "512", "rsize", "700", "seek", "1000" };
    sent_count = 0; n = request(pkt, 1, "/readwrite/big", part, 3); rx(42, pkt, n);
    assert(sent_count == 1 && option_is(sent[0], sent_size[0], "rsize", "700") && option_is(sent[0], sent_size[0], "seek", "1000"));
    sent_count = 0; ack(42, 0); assert(sent_count == 1 && sent_size[0] == 516 && !memcmp(sent[0] + 4, body + 1000, 512));
    sent_count = 0; ack(42, 1); assert(sent_count == 1 && sent_size[0] == 4 + 188 && !memcmp(sent[0] + 4, body + 1512, 188));

    /* What is not there, and what is not ours: error 1. */
    sent_count = 0; n = request(pkt, 1, "/readwrite/absent", NULL, 0); rx(43, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 5 && sent[0][3] == 1);
    sent_count = 0; n = request(pkt, 1, "/readonly/firmware/image/x.mbn", mpss, 3); rx(44, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 5 && sent[0][3] == 1);
    sent_count = 0; n = request(pkt, 2, "/readonly/firmware/image/x.mbn", mpss, 3); rx(45, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 5);
    /* The firmware volume: read whole once, served by pieces over several requests, never written. */
    for (unsigned i = 0; i < sizeof(volume_file); i++) volume_file[i] = (uint8_t)(i * 13 + 1);
    const char *piece[] = { "blksize", "15872", "wsize", "10", "rsize", "9000", "seek", "20000", "tsize", "0" };
    sent_count = 0; n = request(pkt, 1, "/readonly/firmware/image/modem_pr/mcfg/a.mbn", piece, 5); rx(50, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 6 && option_is(sent[0], sent_size[0], "tsize", "30000") && volume_reads == 1);
    assert(option_is(sent[0], sent_size[0], "blksize", "7168"));
    sent_count = 0; ack(50, 0);
    assert(sent_count == 2 && sent_size[0] == 4 + 7168 && sent_size[1] == 4 + 9000 - 7168);
    assert(!memcmp(sent[0] + 4, volume_file + 20000, 7168) && !memcmp(sent[1] + 4, volume_file + 27168, 1832));
    ack(50, 2);
    sent_count = 0; n = request(pkt, 1, "/readonly/firmware/image/modem_pr/mcfg/a.mbn", piece, 5); rx(51, pkt, n);
    assert(sent_count == 1 && volume_reads == 1);
    /* A stat: the size, then the client's "end of transfer". Four more files turn the kept ones over. */
    uint8_t end_of_transfer[5] = { 0, 5, 0, 9, 0 }; rx(51, end_of_transfer, 5);
    const char *others[] = { "/readonly/firmware/image/b.mbn", "/readonly/firmware/image/c.mbn",
        "/readonly/firmware/image/d.mbn", "/readonly/firmware/image/e.mbn" };
    for (unsigned i = 0; i < 4; i++) {
        sent_count = 0; n = request(pkt, 1, others[i], NULL, 0); rx(60 + i, pkt, n);
        assert(sent_count == 1 && sent[0][1] == 3 && volume_reads == 2 + i);
        rx(60 + i, end_of_transfer, 5);
    }
    sent_count = 0; n = request(pkt, 1, "/readonly/firmware/image/modem_pr/mcfg/a.mbn", NULL, 0); rx(70, pkt, n);
    assert(sent_count == 1 && volume_reads == 6);
    sent_count = 0; n = request(pkt, 2, "/readonly/firmware/image/b.mbn", NULL, 0); rx(71, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 5);
    /* No intent for a block: nothing is lost, the loop's poll carries the window on in order. */
    const char *whole[] = { "blksize", "15872", "wsize", "10" };
    paced = true; big_intent = false; link_polls = refused = 0;
    sent_count = 0; n = request(pkt, 1, "/readonly/firmware/image/modem_pr/mcfg/a.mbn", whole, 2); rx(80, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 6);
    sent_count = 0; ack(80, 0); assert(sent_count == 0 && refused == 1);
    annibale_tftp_poll(link_poll);
    assert(sent_count == 5 && link_polls == 5 && sent_size[4] == 4 + 30000 - 4 * 7168);
    for (unsigned i = 0; i < 5; i++) assert(sent[i][3] == i + 1 && !memcmp(sent[i] + 4, volume_file + i * 7168, sent_size[i] - 4));
    sent_count = 0; annibale_tftp_poll(link_poll); assert(sent_count == 0 && link_polls == 5);
    /* An acknowledgement of an older block: from the one after it again. */
    ack(80, 2); annibale_tftp_poll(link_poll);
    assert(sent_count == 3 && sent[0][3] == 3 && sent[2][3] == 5);
    sent_count = 0; ack(80, 5); annibale_tftp_poll(link_poll); assert(sent_count == 0);
    paced = false;
    /* Data nobody asked for is dropped; a file over the limit ends with error 3. */
    sent_count = 0; data_block(99, 1, body, 10); ack(99, 1); assert(sent_count == 0);
    const char *huge[] = { "blksize", "7168" };
    n = request(pkt, 2, "/readwrite/huge", huge, 1); rx(46, pkt, n);
    for (unsigned block = 1; block <= 147; block++) { sent_count = 0; data_block(46, block, body, 7168); }
    assert(sent_count == 1 && sent[0][1] == 5 && sent[0][3] == 3);

    /* ADSP node 5 sensor RO/RW paths + zero-byte sensor file. */
    sent_count = 0; n = request(pkt, 1, "/readonly/vendor/etc/sensors/sns_reg_config", NULL, 0); rx_node(5u, 110, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent_size[0] == 4 + sizeof(k_sns_reg) - 1u);
    assert(memcmp(sent[0] + 4, k_sns_reg, sizeof(k_sns_reg) - 1u) == 0);

    sent_count = 0; n = request(pkt, 1, "/readonly/hlos_rfs/shared/sensors/sns_reg_config", NULL, 0); rx_node(5u, 111, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent_size[0] == 4 + sizeof(k_hlos_sns) - 1u);
    assert(memcmp(sent[0] + 4, k_hlos_sns, sizeof(k_hlos_sns) - 1u) == 0);

    sent_count = 0; n = request(pkt, 1, "/readonly/vendor/etc/sensors/config/sensors_list.txt", NULL, 0); rx_node(5u, 112, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent_size[0] == 4 + sizeof(k_sns_list) - 1u);

    sent_count = 0; n = request(pkt, 1, "/readonly/sensors/hwMisList.txt", NULL, 0); rx_node(5u, 113, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent_size[0] == 4);

    sent_count = 0; n = request(pkt, 2, "/mnt/vendor/persist/sensors/cam_registry_dump.txt", NULL, 0); rx_node(5u, 114, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 4);
    uint8_t wr[4 + 5] = { 0, 3, 0, 1, 's', 'n', 's', 'o', 'k' };
    sent_count = 0; rx_node(5u, 114, wr, sizeof(wr));
    assert(sent_count == 1 && sent[0][1] == 4 && sent[0][3] == 1);
    sent_count = 0; n = request(pkt, 1, "/mnt/vendor/persist/sensors/cam_registry_dump.txt", NULL, 0); rx_node(5u, 115, pkt, n);
    assert(sent_count == 1 && sent[0][1] == 3 && sent_size[0] == 4 + 5 && memcmp(sent[0] + 4, "snsok", 5) == 0);

    puts("TFTP: dual-instance (0+2), server check, windows, rsize/seek, firmware/sensor RO+RW, intent polling, and limits: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="modem-tftp-") as directory:
    source = Path(directory) / "test.c"
    source.write_text(HARNESS.replace("@SOURCE@", SOURCE))
    exe = Path(directory) / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

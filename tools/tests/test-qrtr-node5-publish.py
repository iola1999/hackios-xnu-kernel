#!/usr/bin/env python3
"""Host ASan+UBSan test for QRTR local service broadcast to ADSP node 5 (both orderings)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ANN = ROOT / "xnu/files/bsd/dev/annibale"

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int lck_mtx_t;
typedef void *thread_t;
#define LCK_GRP_DECLARE(name, s) int name
#define LCK_MTX_DECLARE(name, grp) int name
#define Z_WAITOK 0
#define Z_ZERO 1
static void *kalloc_data(size_t sz, int fl) { (void)fl; return calloc(1, sz); }
static void kfree_data(void *p, size_t sz) { (void)sz; free(p); }
static void annibale_wifi_say(const char *fmt, ...) { (void)fmt; }
static void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
static void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
static thread_t current_thread(void) { return (thread_t)0x1234; }
static bool PE_parse_boot_argn(const char *k, void *v, unsigned s) { (void)k; (void)v; (void)s; return false; }
#define bzero(p, n) memset((p), 0, (n))
#define _ANNIBALE_QRTR_H_

struct annibale_qrtr_port;
typedef void (*annibale_qrtr_receive_t)(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size);
typedef void (*annibale_qrtr_server_t)(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port);
struct annibale_qrtr_ops {
    struct annibale_qrtr_port *(*open)(annibale_qrtr_receive_t receive, annibale_qrtr_server_t server, void *context);
    int (*send)(struct annibale_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t size);
    int (*lookup)(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
    void (*close)(struct annibale_qrtr_port *port);
};
struct annibale_qrtr_counts { uint32_t peer, hellos, in, out, services; };
extern const struct annibale_qrtr_ops annibale_qrtr;
int annibale_qrtr_publish(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
void annibale_qrtr_attach_node(uint32_t peer_node, int (*send)(const void *data, uint32_t length));
void annibale_qrtr_input(const void *data, uint32_t length);

#include <stdlib.h>
#include "qrtr_prod.inc"

static unsigned node5_hellos, node5_new_srv_tftp, node5_new_srv_servreg, node5_lookup_hits, node5_lookup_end;

static int fake_node5_send(const void *data, uint32_t len) {
    assert(len >= sizeof(struct qrtr_header) + sizeof(struct qrtr_control));
    struct qrtr_header h;
    struct qrtr_control c;
    memcpy(&h, data, sizeof(h));
    memcpy(&c, (const uint8_t *)data + sizeof(h), sizeof(c));
    assert(h.to_node == 5u);
    if (h.type == QRTR_HELLO) {
        node5_hellos++;
    } else if (h.type == QRTR_NEW_SERVER) {
        if (c.words[0] == 0x1000u) node5_new_srv_tftp++;
        if (c.words[0] == 0x40u) node5_new_srv_servreg++;
    } else if (h.type == QRTR_DATA && h.to_port == 99u) {
        if (c.words[0] == 0x1000u) node5_lookup_hits++;
        else if (c.words[0] == 0u) node5_lookup_end++;
    }
    return 0;
}

static void dummy_rx(void *ctx, uint32_t n, uint32_t p, const void *d, size_t s) {
    (void)ctx; (void)n; (void)p; (void)d; (void)s;
}

int main(void) {
    /* Case 1: publish dual TFTP (0x1000 inst 0+2) BEFORE attaching node 5 -> HELLO from node 5 announces both. */
    struct annibale_qrtr_port *tftp = annibale_qrtr.open(dummy_rx, NULL, NULL);
    assert(tftp != NULL);
    assert(annibale_qrtr_publish(tftp, 0x1000u, 1u, 0u) == 0);
    assert(annibale_qrtr_publish(tftp, 0x1000u, 1u, 2u) == 0);
    annibale_qrtr_attach_node(5u, fake_node5_send);
    assert(node5_hellos == 1u);
    uint8_t hello_pkt[sizeof(struct qrtr_header) + sizeof(struct qrtr_control)];
    struct qrtr_header hh = {
        .version = 1, .type = QRTR_HELLO, .from_node = 5u, .from_port = QRTR_CONTROL,
        .size = sizeof(struct qrtr_control), .to_node = QRTR_US, .to_port = QRTR_CONTROL,
    };
    struct qrtr_control hc = { .command = QRTR_HELLO };
    memcpy(hello_pkt, &hh, sizeof(hh));
    memcpy(hello_pkt + sizeof(hh), &hc, sizeof(hc));
    annibale_qrtr_input(hello_pkt, sizeof(hello_pkt));
    assert(node5_hellos == 2u);
    assert(node5_new_srv_tftp == 2u);

    /* Case 2: publish SERVREG_LOC (0x40) AFTER node 5 HELLO -> broadcast immediately to node 5. */
    struct annibale_qrtr_port *servreg = annibale_qrtr.open(dummy_rx, NULL, NULL);
    assert(servreg != NULL);
    assert(annibale_qrtr_publish(servreg, 0x40u, 1u, 1u) == 0);
    assert(node5_new_srv_servreg == 1u);

    /* Case 3: node 5 sends QRTR_NEW_LOOKUP for TFTP (0x1000) -> gets matching servers + zero terminator. */
    uint8_t pkt[sizeof(struct qrtr_header) + sizeof(struct qrtr_control)];
    struct qrtr_header h = {
        .version = 1, .type = QRTR_NEW_LOOKUP, .from_node = 5u, .from_port = 99u,
        .size = sizeof(struct qrtr_control), .to_node = QRTR_US, .to_port = QRTR_CONTROL,
    };
    struct qrtr_control c = { .command = QRTR_NEW_LOOKUP, .words = { 0x1000u, 0u, 0u, 0u } };
    memcpy(pkt, &h, sizeof(h));
    memcpy(pkt + sizeof(h), &c, sizeof(c));
    annibale_qrtr_input(pkt, sizeof(pkt));
    assert(node5_lookup_hits == 2u && node5_lookup_end == 1u);

    /* Case 4: lookup filtered specifically to instance=2 (wire = ver | (2 << 8)) returns 1 hit + end. */
    node5_lookup_hits = node5_lookup_end = 0;
    c.words[1] = 1u | (2u << 8);
    memcpy(pkt + sizeof(h), &c, sizeof(c));
    annibale_qrtr_input(pkt, sizeof(pkt));
    assert(node5_lookup_hits == 1u && node5_lookup_end == 1u);

    puts("QRTR node 5 local service publish (before/after attach) and lookup: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="qrtr-node5-") as d:
    tmp = Path(d)
    prod = (ANN / "annibale_qrtr.c").read_text()
    stripped = "\n".join(
        line for line in prod.splitlines()
        if not line.lstrip().startswith("#include <") and not line.lstrip().startswith('#include "')
    )
    (tmp / "qrtr_prod.inc").write_text(stripped + "\n")
    src = tmp / "test.c"
    exe = tmp / "test"
    src.write_text(HARNESS)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(tmp), "-I", str(ANN), str(src), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)

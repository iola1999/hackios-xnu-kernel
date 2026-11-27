#!/usr/bin/env python3
"""Host ASan+UBSan unit tests for annibale_modem_uim.inc (P13 multi-slot QMI_UIM decoder)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UIM_INC = ROOT / "xnu/files/bsd/dev/annibale/annibale_modem_uim.inc"

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct { uint8_t uim_card, uim_err; } modem_loc_diag = { 0xffu, 0xffu };

static const uint8_t *
annibale_qmi_find_tlv(const uint8_t *tlvs, uint16_t tlv_len, uint8_t type, uint16_t *len_out)
{
    size_t at = 0;
    while (at + 3u <= tlv_len) {
        uint8_t t = tlvs[at];
        uint16_t l = (uint16_t)(tlvs[at + 1u] | ((uint16_t)tlvs[at + 2u] << 8));
        at += 3u;
        if (at + l > tlv_len) break;
        if (t == type) {
            if (len_out) *len_out = l;
            return tlvs + at;
        }
        at += l;
    }
    if (len_out) *len_out = 0;
    return NULL;
}

static bool
svc_qmi_read_result(const uint8_t *tlvs, uint16_t tlv_len, uint16_t *res, uint16_t *err)
{
    uint16_t l = 0;
    const uint8_t *v = annibale_qmi_find_tlv(tlvs, tlv_len, 0x02u, &l);
    if (v == NULL || l < 4u) return false;
    *res = (uint16_t)(v[0] | ((uint16_t)v[1] << 8));
    *err = (uint16_t)(v[2] | ((uint16_t)v[3] << 8));
    return true;
}

__UIM_INC__

int main(void) {
    uint32_t sim = 99u;

    /* 1. Dual-slot GET_CARD_STATUS (0x002f): slot0 absent (0 apps), slot1 present+ready (app_state=7, aid_len=4) */
    uint8_t card_val[9 + 6 + 6 + 18] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, /* 2 slots */
        0, 0, 0, 0, 1, 0,                                  /* slot0: absent, err=1, 0 apps */
        1, 2, 3, 3, 0, 1,                                  /* slot1: present, 1 app */
        2, 7, 0, 0, 0, 0, 4, 0xa0, 0x00, 0x00, 0x01, 0, 2, 3, 10, 2, 3, 10
    };
    uint8_t msg1[7 + 3 + sizeof(card_val)] = { 0x02, 4, 0, 0, 0, 0, 0, 0x10, sizeof(card_val), 0 };
    memcpy(msg1 + 10, card_val, sizeof(card_val));
    modem_uim_handle_rx(0x02u, 0x002fu, msg1, sizeof(msg1), &sim);
    assert(sim == 2u && modem_loc_diag.uim_card == 1u && modem_loc_diag.uim_err == 0u);

    /* 2. Dual-slot with slot0 having 1 app (aid_len=3, state=2) and slot1 having 1 app (aid_len=2, state=3) */
    uint8_t card_val2[9 + 6 + 17 + 6 + 16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 2, 1,
        1, 2, 0, 0, 0, 0, 3, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 0, 0, 0, 1,
        2, 3, 0, 0, 0, 0, 2, 9, 8, 0, 0, 0, 0, 0, 0, 0,
    };
    uint8_t msg2[3 + sizeof(card_val2)] = { 0x10, sizeof(card_val2), 0 };
    memcpy(msg2 + 3, card_val2, sizeof(card_val2));
    modem_uim_handle_rx(0x04u, 0x0032u, msg2, sizeof(msg2), &sim);
    assert(sim == 1u && modem_loc_diag.uim_card == 1u && modem_loc_diag.uim_err == 0u);

    /* 3. GET_SLOTS_STATUS (0x0047): 2 physical slots, slot0 absent, slot1 present with 10B iccid */
    uint8_t phys_val[1 + 4 + 14] = {
        2,
        1, 1, 1, 0,
        2, 1, 2, 10, 0x98, 0x68, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    uint8_t msg3[7 + 3 + sizeof(phys_val)] = { 0x02, 4, 0, 0, 0, 0, 0, 0x10, sizeof(phys_val), 0 };
    memcpy(msg3 + 10, phys_val, sizeof(phys_val));
    modem_uim_handle_rx(0x02u, 0x0047u, msg3, sizeof(msg3), &sim);

    /* 4. EVENT_REG (0x002e) response */
    uint8_t msg4[7] = { 0x02, 4, 0, 0, 0, 0, 0 };
    modem_uim_handle_rx(0x02u, 0x002eu, msg4, sizeof(msg4), &sim);

    /* 5. Truncated / malformed fuzzing under ASan+UBSan */
    for (uint16_t n = 0; n <= sizeof(msg1); n++) {
        modem_uim_handle_rx(0x02u, 0x002fu, msg1, n, &sim);
    }
    for (uint16_t n = 0; n <= sizeof(msg3); n++) {
        modem_uim_handle_rx(0x02u, 0x0047u, msg3, n, &sim);
    }

    printf("PASS: annibale_modem_uim multi-slot 0x002e/0x002f/0x0032/0x0047 (ASan+UBSan)\n");
    return 0;
}
"""


def main() -> None:
    code = HARNESS.replace("__UIM_INC__", UIM_INC.read_text())
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test_modem_uim.c"
        exe = Path(td) / "test_modem_uim"
        src.write_text(code)
        subprocess.run(
            ["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             str(src), "-o", str(exe)],
            check=True,
        )
        res = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
        print(res.stdout.strip().splitlines()[-1])


if __name__ == "__main__":
    main()

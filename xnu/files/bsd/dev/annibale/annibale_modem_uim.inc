/*
 * Multi-slot QMI_UIM (0x0b) decoder for GET_CARD_STATUS (0x002f), STATUS_CHANGE_IND (0x0032),
 * GET_SLOTS_STATUS (0x0047), and SLOT_STATUS_CHANGE_IND (0x0048).
 *
 * Included by annibale_modem_svc.inc. Never logs ICCID/IMSI bytes (AGENTS.md §3).
 */
#ifndef _ANNIBALE_MODEM_UIM_INC_
#define _ANNIBALE_MODEM_UIM_INC_

static void
modem_uim_parse_card_slots(const uint8_t *cv, uint16_t clen, uint32_t *sim_state_out)
{
	if (cv == NULL || clen < 9u) {
		return;
	}
	uint8_t num_slots = cv[8];
	size_t off = 9u;
	uint32_t best_sim = 0;
	uint8_t rep_card = 0, rep_err = 0;
	bool have_slot = false;

	for (uint8_t s = 0; s < num_slots && s < 4u && off + 6u <= clen; s++) {
		uint8_t card = cv[off + 0u];
		uint8_t upin = cv[off + 1u];
		uint8_t uim_err = cv[off + 4u];
		uint8_t apps = cv[off + 5u];
		uint8_t best_app_st = 0;
		off += 6u;

		for (uint8_t a = 0; a < apps && off + 7u <= clen; a++) {
			uint8_t ast = cv[off + 1u];
			uint8_t aid_len = cv[off + 6u];
			if ((size_t)14u + aid_len > (size_t)clen - off) {
				off = clen;
				break;
			}
			if (ast == 7u || best_app_st == 0u) {
				best_app_st = ast;
			}
			off += (size_t)14u + aid_len;
		}
		uint32_t slot_sim = (card == 1u) ? (best_app_st == 7u ? 2u : 1u) : 0u;
		if (!have_slot || slot_sim > best_sim) {
			have_slot = true;
			best_sim = slot_sim;
			rep_card = card;
			rep_err = uim_err;
		}
		printf("annibale-modem: QMI UIM slots=%u slot%u=(card=%u upin=%u err=%u apps=%u app_st=%u) sim=%u (subscriber_id=***)\n",
		    num_slots, s, card, upin, uim_err, apps, best_app_st, slot_sim);
	}
	if (have_slot) {
		modem_loc_diag.uim_card = rep_card;
		modem_loc_diag.uim_err = rep_err;
		*sim_state_out = best_sim;
	}
}

static void
modem_uim_parse_phys_slots(const uint8_t *sv, uint16_t slen)
{
	if (sv == NULL || slen < 1u) {
		return;
	}
	uint8_t num_slots = sv[0];
	size_t off = 1u;

	for (uint8_t s = 0; s < num_slots && s < 4u && off + 4u <= slen; s++) {
		uint8_t card_st = sv[off + 0u];
		uint8_t slot_st = sv[off + 1u];
		uint8_t logical = sv[off + 2u];
		uint8_t iccid_len = sv[off + 3u];
		off += 4u;
		if (iccid_len > (size_t)slen - off) {
			break;
		}
		off += iccid_len;
		printf("annibale-modem: QMI UIM phys_slot%u=(card=%u active=%u logical=%u iccid_len=%u)\n",
		    s, card_st, slot_st, logical, iccid_len);
	}
}

static void
modem_uim_handle_rx(uint8_t qmi_flags, uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len,
    uint32_t *sim_state_out)
{
	uint16_t res = 0, err = 0, vlen = 0;

	if (msg_id == 0x002eu /* EVENT_REG */) {
		if (qmi_flags == 0x02u && svc_qmi_read_result(tlvs, tlv_len, &res, &err)) {
			printf("annibale-modem: QMI UIM EVENT_REG res=%u err=%u\n", res, err);
		}
		return;
	}
	if (msg_id == 0x0047u /* GET_SLOTS_STATUS */ || msg_id == 0x0048u /* SLOT_STATUS_CHANGE_IND */) {
		if (qmi_flags == 0x02u && svc_qmi_read_result(tlvs, tlv_len, &res, &err) && res != 0) {
			printf("annibale-modem: QMI UIM GET_SLOTS_STATUS failed res=%u err=%u\n", res, err);
			return;
		}
		modem_uim_parse_phys_slots(annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &vlen), vlen);
		return;
	}
	if (msg_id != 0x002fu /* GET_CARD_STATUS */ && msg_id != 0x0032u /* CARD_STATUS_IND */) {
		return;
	}
	if (qmi_flags == 0x02u && svc_qmi_read_result(tlvs, tlv_len, &res, &err) && res != 0) {
		printf("annibale-modem: QMI UIM GET_CARD_STATUS failed res=%u err=%u\n", res, err);
		return;
	}
	modem_uim_parse_card_slots(annibale_qmi_find_tlv(tlvs, tlv_len, 0x10u, &vlen), vlen, sim_state_out);
}

#endif /* _ANNIBALE_MODEM_UIM_INC_ */

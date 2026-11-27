/*
 * Native ADSP QMI SNS_CLIENT (service 400 / 0x190, node 5) client for ambient_light & proximity.
 *
 * Uses annibale_qrtr directly (independent of Wi-Fi MHI transport setup) to:
 *   1. Look up service 400 (QMI_SNS_CLIENT, v1 inst 0) over ADSP SMEM GLINK IPCRTR.
 *   2. Query SUIDs for "ambient_light" and "proximity" via SNS_SUID_REQ (msg_id 512).
 *   3. Subscribe to discovered SUIDs via SNS_STD_ON_CHANGE_CONFIG (msg_id 514).
 *   4. Decode incoming SNS_STD_SENSOR_EVENT (1025) and SNS_PROXIMITY_EVENT (769) protobuf
 *      indications (QMI_SNS_CLIENT_REPORT_IND 0x0022) without floating-point instructions and
 *      feed annibale_sensors_report() / sensor_state.
 *
 * Included by annibale_sensors.c.
 */

#include <sys/annibale_qrtr.h>

#define QMI_SVC_SNS_CLIENT              400u    /* 0x0190 */
#define QMI_SNS_CLIENT_REQ              0x0020u
#define QMI_SNS_CLIENT_RESP             0x0020u
#define QMI_SNS_CLIENT_REPORT_IND       0x0021u
#define QMI_SNS_CLIENT_JUMBO_IND        0x0022u

#define QMI_HDR_TYPE_REQ                0u
#define QMI_HDR_TYPE_RESP               2u
#define QMI_HDR_TYPE_IND                4u
#define QMI_HDR_LEN                     7u

#define SNS_STD_CLIENT_PROCESSOR_APSS   1u      /* 0=SSC, 1=APSS */
#define SNS_CLIENT_DELIVERY_WAKEUP      0u

#define SNS_SUID_LOOKUP_LOW             0xababababababababULL
#define SNS_SUID_LOOKUP_HIGH            0xababababababababULL
#define SNS_SUID_MSGID_REQ              512u
#define SNS_SUID_MSGID_EVENT            768u
#define SNS_STD_SENSOR_MSGID_ON_CHANGE  514u
#define SNS_PROXIMITY_MSGID_EVENT       769u
#define SNS_STD_SENSOR_MSGID_EVENT      1025u

struct ssc_suid {
	uint64_t low;
	uint64_t high;
	bool     valid;
};

static struct annibale_qrtr_port *ssc_port;
static uint32_t ssc_srv_node, ssc_srv_port;
static bool ssc_srv_up;
static bool ssc_need_suid_lookup, ssc_als_sub_pending, ssc_prox_sub_pending;
static uint16_t ssc_txn_id;
static struct ssc_suid ssc_als_suid, ssc_prox_suid;
static uint32_t ssc_resp_logs, ssc_als_logs, ssc_prox_logs, ssc_ind_count, ssc_rx_diag_logs;

/* Integer-only IEEE-754 binary32 -> non-negative integer conversion (safe in XNU kernel context). */
static uint32_t
ssc_f32_bits_to_u32(uint32_t bits, uint32_t max_val)
{
	uint32_t sign = bits >> 31;
	uint32_t exp = (bits >> 23) & 0xffu;
	uint32_t frac = bits & 0x7fffffu;

	if (sign != 0 || exp == 0 || exp < 127u) {
		return 0u;
	}
	if (exp == 0xffu) {
		return frac == 0 ? max_val : 0u;
	}
	uint32_t shift = exp - 127u;
	uint32_t mant = (1u << 23) | frac;
	if (shift >= 31u) {
		return max_val;
	}
	uint32_t val = shift >= 23u ? (mant << (shift - 23u)) : (mant >> (23u - shift));
	return val > max_val ? max_val : val;
}

/* ---- Minimal bounds-checked Protobuf encoder --------------------------------------------------- */

struct pb_writer {
	uint8_t *buf;
	size_t   cap;
	size_t   pos;
	bool     overflow;
};

static void
pb_put_byte(struct pb_writer *w, uint8_t b)
{
	if (w->pos < w->cap) {
		w->buf[w->pos++] = b;
	} else {
		w->overflow = true;
	}
}

static void
pb_put_bytes(struct pb_writer *w, const void *src, size_t n)
{
	if (w->pos + n <= w->cap) {
		memcpy(w->buf + w->pos, src, n);
		w->pos += n;
	} else {
		w->overflow = true;
	}
}

static void
pb_put_varint(struct pb_writer *w, uint64_t v)
{
	do {
		uint8_t b = (uint8_t)(v & 0x7fu);
		v >>= 7;
		if (v != 0) {
			b |= 0x80u;
		}
		pb_put_byte(w, b);
	} while (v != 0);
}

static void
pb_put_tag(struct pb_writer *w, uint32_t field, uint32_t wire)
{
	pb_put_varint(w, ((uint64_t)field << 3) | (wire & 7u));
}

static void
pb_put_fixed32(struct pb_writer *w, uint32_t field, uint32_t v)
{
	uint8_t le[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

	pb_put_tag(w, field, 5u);
	pb_put_bytes(w, le, 4u);
}

static void
pb_put_fixed64(struct pb_writer *w, uint32_t field, uint64_t v)
{
	uint8_t le[8];

	for (unsigned i = 0; i < 8u; i++) {
		le[i] = (uint8_t)(v >> (8u * i));
	}
	pb_put_tag(w, field, 1u);
	pb_put_bytes(w, le, 8u);
}

static void
pb_put_len_delim(struct pb_writer *w, uint32_t field, const void *data, size_t len)
{
	pb_put_tag(w, field, 2u);
	pb_put_varint(w, len);
	pb_put_bytes(w, data, len);
}

/* ---- Minimal bounds-checked Protobuf decoder --------------------------------------------------- */

struct pb_reader {
	const uint8_t *buf;
	size_t         len;
	size_t         pos;
};

static bool
pb_get_varint(struct pb_reader *r, uint64_t *out)
{
	uint64_t v = 0;
	unsigned shift = 0;

	while (r->pos < r->len && shift < 64u) {
		uint8_t b = r->buf[r->pos++];
		v |= (uint64_t)(b & 0x7fu) << shift;
		if ((b & 0x80u) == 0) {
			*out = v;
			return true;
		}
		shift += 7u;
	}
	return false;
}

static bool
pb_get_fixed32(struct pb_reader *r, uint32_t *out)
{
	if (r->len - r->pos < 4u) {
		return false;
	}
	uint32_t v = 0;
	memcpy(&v, r->buf + r->pos, 4u);
	r->pos += 4u;
	*out = v;
	return true;
}

static bool
pb_get_fixed64(struct pb_reader *r, uint64_t *out)
{
	if (r->len - r->pos < 8u) {
		return false;
	}
	uint64_t v = 0;
	memcpy(&v, r->buf + r->pos, 8u);
	r->pos += 8u;
	*out = v;
	return true;
}

static bool
pb_skip_field(struct pb_reader *r, uint32_t wire)
{
	uint64_t v = 0;

	if (wire == 0u) {
		return pb_get_varint(r, &v);
	}
	if (wire == 1u) {
		if (r->len - r->pos < 8u) return false;
		r->pos += 8u;
		return true;
	}
	if (wire == 2u) {
		if (!pb_get_varint(r, &v) || v > r->len - r->pos) return false;
		r->pos += (size_t)v;
		return true;
	}
	if (wire == 5u) {
		if (r->len - r->pos < 4u) return false;
		r->pos += 4u;
		return true;
	}
	return false;
}

/* ---- SNS_CLIENT request encoding & QMI send ---------------------------------------------------- */

static size_t
ssc_encode_client_req(uint8_t *out, size_t cap, uint64_t suid_low, uint64_t suid_high,
    uint32_t msg_id, const uint8_t *inner_payload, size_t inner_len)
{
	uint8_t suid_buf[24];
	struct pb_writer sw = { suid_buf, sizeof(suid_buf), 0, false };
	pb_put_fixed64(&sw, 1u, suid_low);
	pb_put_fixed64(&sw, 2u, suid_high);

	uint8_t susp_buf[8];
	struct pb_writer cw = { susp_buf, sizeof(susp_buf), 0, false };
	pb_put_tag(&cw, 1u, 0u); pb_put_varint(&cw, SNS_STD_CLIENT_PROCESSOR_APSS);
	pb_put_tag(&cw, 2u, 0u); pb_put_varint(&cw, SNS_CLIENT_DELIVERY_WAKEUP);

	uint8_t std_buf[128];
	struct pb_writer rw = { std_buf, sizeof(std_buf), 0, false };
	if (inner_payload != NULL && inner_len > 0) {
		pb_put_len_delim(&rw, 2u, inner_payload, inner_len);
	}

	struct pb_writer mw = { out, cap, 0, false };
	pb_put_len_delim(&mw, 1u, suid_buf, sw.pos);
	pb_put_fixed32(&mw, 2u, msg_id);
	pb_put_len_delim(&mw, 3u, susp_buf, cw.pos);
	pb_put_len_delim(&mw, 4u, std_buf, rw.pos);
	return (sw.overflow || cw.overflow || rw.overflow || mw.overflow) ? 0u : mw.pos;
}

static size_t
ssc_encode_qmi_req(uint8_t *qmi, size_t cap, uint16_t txn, const uint8_t *pb, size_t pb_len)
{
	/* 7B QMI header + TLV 0x01 (3B hdr + 2B len + pb_len) + TLV 0x10 (3B hdr + 1B use_jumbo=0) */
	size_t tlv1_len = 2u + pb_len;
	size_t body_len = (3u + tlv1_len) + 4u;
	size_t total = QMI_HDR_LEN + body_len;
	uint16_t msg_id = QMI_SNS_CLIENT_REQ;
	uint16_t blen16 = (uint16_t)body_len;
	uint16_t t1len16 = (uint16_t)tlv1_len;
	uint16_t plen16 = (uint16_t)pb_len;

	if (total > cap || pb_len > 998u) {
		return 0u;
	}
	qmi[0] = QMI_HDR_TYPE_REQ;
	memcpy(qmi + 1, &txn, 2u);
	memcpy(qmi + 3, &msg_id, 2u);
	memcpy(qmi + 5, &blen16, 2u);
	size_t at = QMI_HDR_LEN;
	qmi[at++] = 0x01u;
	memcpy(qmi + at, &t1len16, 2u); at += 2u;
	memcpy(qmi + at, &plen16, 2u); at += 2u;
	memcpy(qmi + at, pb, pb_len); at += pb_len;
	qmi[at++] = 0x10u;
	qmi[at++] = 0x01u;
	qmi[at++] = 0x00u;
	qmi[at++] = 0x00u;
	return at;
}

static int
ssc_send_pb(uint64_t suid_low, uint64_t suid_high, uint32_t msg_id, const uint8_t *inner, size_t inner_len)
{
	uint8_t pb[192], qmi[256];

	if (ssc_port == NULL || !ssc_srv_up) {
		return ENOTCONN;
	}
	size_t pb_len = ssc_encode_client_req(pb, sizeof(pb), suid_low, suid_high, msg_id, inner, inner_len);
	if (pb_len == 0) {
		return EINVAL;
	}
	uint16_t txn = ++ssc_txn_id;
	size_t qmi_len = ssc_encode_qmi_req(qmi, sizeof(qmi), txn, pb, pb_len);
	if (qmi_len == 0) {
		return EINVAL;
	}
	return annibale_qrtr.send(ssc_port, ssc_srv_node, ssc_srv_port, qmi, qmi_len);
}

static void
ssc_send_suid_lookup(const char *data_type)
{
	uint8_t inner[48];
	struct pb_writer w = { inner, sizeof(inner), 0, false };

	pb_put_len_delim(&w, 1u, data_type, strlen(data_type));
	pb_put_tag(&w, 2u, 0u);
	pb_put_varint(&w, 1u); /* register_updates = true */
	if (w.overflow) {
		return;
	}
	int err = ssc_send_pb(SNS_SUID_LOOKUP_LOW, SNS_SUID_LOOKUP_HIGH, SNS_SUID_MSGID_REQ, inner, w.pos);
	say("sensors-ssc: SUID lookup \"%s\" -> err=%d\n", data_type, err);
}

/* ---- SNS_CLIENT event decoding ----------------------------------------------------------------- */

static bool
ssc_decode_suid(const uint8_t *buf, size_t len, struct ssc_suid *out)
{
	struct pb_reader r = { buf, len, 0 };
	bool has_lo = false, has_hi = false;

	out->low = out->high = 0;
	out->valid = false;
	while (r.pos < r.len) {
		uint64_t tag = 0;
		if (!pb_get_varint(&r, &tag)) return false;
		uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7u);
		if (field == 1u && wire == 1u) {
			if (!pb_get_fixed64(&r, &out->low)) return false;
			has_lo = true;
		} else if (field == 2u && wire == 1u) {
			if (!pb_get_fixed64(&r, &out->high)) return false;
			has_hi = true;
		} else if (!pb_skip_field(&r, wire)) {
			return false;
		}
	}
	out->valid = has_lo && has_hi;
	return out->valid;
}

static void
ssc_handle_suid_event(const uint8_t *buf, size_t len)
{
	struct pb_reader r = { buf, len, 0 };
	char dtype[32] = { 0 };
	struct ssc_suid first = { 0, 0, false };
	uint32_t count = 0;

	while (r.pos < r.len) {
		uint64_t tag = 0, flen = 0;
		if (!pb_get_varint(&r, &tag)) break;
		uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7u);
		if (field == 1u && wire == 2u) {
			if (!pb_get_varint(&r, &flen) || flen > r.len - r.pos) break;
			size_t n = flen < sizeof(dtype) - 1u ? (size_t)flen : sizeof(dtype) - 1u;
			memcpy(dtype, r.buf + r.pos, n);
			dtype[n] = '\0';
			r.pos += (size_t)flen;
		} else if (field == 2u && wire == 2u) {
			if (!pb_get_varint(&r, &flen) || flen > r.len - r.pos) break;
			struct ssc_suid s;
			if (ssc_decode_suid(r.buf + r.pos, (size_t)flen, &s)) {
				if (count == 0) {
					first = s;
				}
				count++;
			}
			r.pos += (size_t)flen;
		} else if (!pb_skip_field(&r, wire)) {
			break;
		}
	}
	say("sensors-ssc: SUID event \"%s\" count=%u first=0x%016llx:%016llx\n",
	    dtype, count, (unsigned long long)first.high, (unsigned long long)first.low);
	if (!first.valid) {
		return;
	}
	/* Record discovered SUIDs under sensors_lck; actual QRTR send runs in sensors_ssc_tick()
	 * outside qrtr_calling to avoid self-deadlock if glink_take_their_intent() polls RX. */
	lck_mtx_lock(&sensors_lck);
	if (strcmp(dtype, "ambient_light") == 0 && !ssc_als_suid.valid) {
		ssc_als_suid = first;
		ssc_als_sub_pending = true;
	} else if (strcmp(dtype, "proximity") == 0 && !ssc_prox_suid.valid) {
		ssc_prox_suid = first;
		ssc_prox_sub_pending = true;
	}
	lck_mtx_unlock(&sensors_lck);
}

static void
ssc_handle_std_sensor_event(const struct ssc_suid *uid, const uint8_t *buf, size_t len)
{
	struct pb_reader r = { buf, len, 0 };
	uint32_t f32[4] = { 0 };
	uint32_t n = 0;
	uint64_t status = 0;

	while (r.pos < r.len) {
		uint64_t tag = 0, plen = 0;
		if (!pb_get_varint(&r, &tag)) break;
		uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7u);
		if (field == 1u && wire == 5u) {
			uint32_t v = 0;
			if (!pb_get_fixed32(&r, &v)) break;
			if (n < 4u) f32[n++] = v;
		} else if (field == 1u && wire == 2u) {
			if (!pb_get_varint(&r, &plen) || plen > r.len - r.pos) break;
			size_t end = r.pos + (size_t)plen;
			while (r.pos + 4u <= end) {
				uint32_t v = 0;
				(void)pb_get_fixed32(&r, &v);
				if (n < 4u) f32[n++] = v;
			}
			r.pos = end;
		} else if (field == 2u && wire == 0u) {
			if (!pb_get_varint(&r, &status)) break;
		} else if (!pb_skip_field(&r, wire)) {
			break;
		}
	}
	if (n == 0) {
		return;
	}
	if (ssc_als_suid.valid && uid->low == ssc_als_suid.low && uid->high == ssc_als_suid.high) {
		uint32_t lux = ssc_f32_bits_to_u32(f32[0], 100000u);
		uint32_t raw = n >= 2u ? ssc_f32_bits_to_u32(f32[1], 1000000u) : lux * 4u;
		lck_mtx_lock(&sensors_lck);
		sensor_state.sample.als_lux = lux;
		sensor_state.sample.clear_raw = raw;
		sensor_state.sample.source_flags |= ANNIBALE_SENSOR_SRC_ADSP_SSC | ANNIBALE_SENSOR_SRC_FRONT_OK;
		sensor_state.sample.timestamp = mach_absolute_time();
		lck_mtx_unlock(&sensors_lck);
		if (ssc_als_logs < 8u) {
			ssc_als_logs++;
			say("sensors-ssc: ambient_light lux=%u raw=%u status=%llu (n=%u)\n",
			    lux, raw, (unsigned long long)status, n);
		}
	}
}

static void
ssc_handle_prox_event(const uint8_t *buf, size_t len)
{
	struct pb_reader r = { buf, len, 0 };
	uint64_t ptype = 0, raw_adc = 0, status = 0;
	bool has_type = false;

	while (r.pos < r.len) {
		uint64_t tag = 0;
		if (!pb_get_varint(&r, &tag)) break;
		uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7u);
		if (field == 1u && wire == 0u) {
			if (!pb_get_varint(&r, &ptype)) break;
			has_type = true;
		} else if (field == 2u && wire == 0u) {
			if (!pb_get_varint(&r, &raw_adc)) break;
		} else if (field == 3u && wire == 0u) {
			if (!pb_get_varint(&r, &status)) break;
		} else if (!pb_skip_field(&r, wire)) {
			break;
		}
	}
	if (!has_type) {
		return;
	}
	uint8_t near = ptype == 1u ? 1u : 0u;
	lck_mtx_lock(&sensors_lck);
	sensor_state.sample.prox_near = near;
	sensor_state.sample.prox_distance_mm = near ? 0u : 50u;
	if (raw_adc != 0) {
		sensor_state.sample.prox_raw = (uint32_t)raw_adc;
	}
	sensor_state.sample.timestamp = mach_absolute_time();
	lck_mtx_unlock(&sensors_lck);
	if (ssc_prox_logs < 8u) {
		ssc_prox_logs++;
		say("sensors-ssc: proximity near=%u raw_adc=%llu status=%llu\n",
		    near, (unsigned long long)raw_adc, (unsigned long long)status);
	}
}

static void
ssc_decode_event_msg(const uint8_t *pb, size_t pb_len)
{
	struct pb_reader r = { pb, pb_len, 0 };
	struct ssc_suid uid = { 0, 0, false };

	while (r.pos < r.len) {
		uint64_t tag = 0, flen = 0;
		if (!pb_get_varint(&r, &tag)) break;
		uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7u);
		if (field == 1u && wire == 2u) {
			if (!pb_get_varint(&r, &flen) || flen > r.len - r.pos) break;
			(void)ssc_decode_suid(r.buf + r.pos, (size_t)flen, &uid);
			r.pos += (size_t)flen;
		} else if (field == 2u && wire == 2u) {
			if (!pb_get_varint(&r, &flen) || flen > r.len - r.pos) break;
			struct pb_reader er = { r.buf + r.pos, (size_t)flen, 0 };
			uint32_t msg_id = 0;
			const uint8_t *epayload = NULL;
			size_t epayload_len = 0;
			while (er.pos < er.len) {
				uint64_t etag = 0, ev = 0;
				if (!pb_get_varint(&er, &etag)) break;
				uint32_t ef = (uint32_t)(etag >> 3), ew = (uint32_t)(etag & 7u);
				if (ef == 1u && ew == 5u) {
					if (!pb_get_fixed32(&er, &msg_id)) break;
				} else if (ef == 1u && ew == 0u) {
					if (!pb_get_varint(&er, &ev)) break;
					msg_id = (uint32_t)ev;
				} else if (ef == 3u && ew == 2u) {
					if (!pb_get_varint(&er, &ev) || ev > er.len - er.pos) break;
					epayload = er.buf + er.pos;
					epayload_len = (size_t)ev;
					er.pos += (size_t)ev;
				} else if (!pb_skip_field(&er, ew)) {
					break;
				}
			}
			if (epayload != NULL) {
				if (msg_id == SNS_SUID_MSGID_EVENT) {
					ssc_handle_suid_event(epayload, epayload_len);
				} else if (msg_id == SNS_STD_SENSOR_MSGID_EVENT) {
					ssc_handle_std_sensor_event(&uid, epayload, epayload_len);
				} else if (msg_id == SNS_PROXIMITY_MSGID_EVENT) {
					ssc_handle_prox_event(epayload, epayload_len);
				} else if (ssc_rx_diag_logs < 16u) {
					ssc_rx_diag_logs++;
					say("sensors-ssc: event msg_id=%u len=%zu suid=0x%016llx:%016llx\n",
					    msg_id, epayload_len, (unsigned long long)uid.high, (unsigned long long)uid.low);
				}
			}
			r.pos += (size_t)flen;
		} else if (!pb_skip_field(&r, wire)) {
			break;
		}
	}
}

static void
ssc_qrtr_rx(void *context __unused, uint32_t node __unused, uint32_t port __unused,
    const void *data, size_t length)
{
	const uint8_t *p = (const uint8_t *)data;
	if (length < QMI_HDR_LEN) {
		return;
	}
	uint8_t type = p[0];
	uint16_t txn = 0, msg_id = 0, body_len = 0;
	memcpy(&txn, p + 1, 2u);
	memcpy(&msg_id, p + 3, 2u);
	memcpy(&body_len, p + 5, 2u);
	if ((size_t)QMI_HDR_LEN + body_len > length) {
		return;
	}
	size_t at = QMI_HDR_LEN, end = QMI_HDR_LEN + body_len;

	if (type == QMI_HDR_TYPE_RESP && msg_id == QMI_SNS_CLIENT_RESP) {
		uint16_t qres = 0, qerr = 0;
		uint32_t sns_err = 0;
		while (at + 3u <= end) {
			uint8_t t = p[at];
			uint16_t l = 0;
			memcpy(&l, p + at + 1, 2u);
			at += 3u;
			if (at + l > end) break;
			if (t == 0x02u && l >= 4u) {
				memcpy(&qres, p + at, 2u);
				memcpy(&qerr, p + at + 2, 2u);
			} else if (t == 0x11u && l >= 4u) {
				memcpy(&sns_err, p + at, 4u);
			}
			at += l;
		}
		if (ssc_resp_logs < 6u) {
			ssc_resp_logs++;
			say("sensors-ssc: QMI resp txn=%u res=%u err=%u sns_err=%u\n", txn, qres, qerr, sns_err);
		}
	} else if (type == QMI_HDR_TYPE_IND &&
	    (msg_id == QMI_SNS_CLIENT_REPORT_IND || msg_id == QMI_SNS_CLIENT_JUMBO_IND)) {
		ssc_ind_count++;
		if (ssc_ind_count <= 8u) {
			say("sensors-ssc: QMI ind #%u msg=0x%04x len=%u\n", ssc_ind_count, msg_id, body_len);
		}
		while (at + 3u <= end) {
			uint8_t t = p[at];
			uint16_t l = 0;
			memcpy(&l, p + at + 1, 2u);
			at += 3u;
			if (at + l > end) break;
			if (t == 0x02u && l >= 2u) {
				uint16_t pb_len = 0;
				memcpy(&pb_len, p + at, 2u);
				if ((size_t)2u + pb_len <= l) {
					ssc_decode_event_msg(p + at + 2u, pb_len);
				}
			}
			at += l;
		}
	} else if (ssc_rx_diag_logs < 16u) {
		ssc_rx_diag_logs++;
		say("sensors-ssc: QMI rx type=%u txn=%u msg=0x%04x len=%u\n", type, txn, msg_id, body_len);
	}
}

static void
ssc_qrtr_server(void *context __unused, bool arrived, uint32_t service, uint32_t version __unused,
    uint32_t instance __unused, uint32_t node, uint32_t port)
{
	if (service != QMI_SVC_SNS_CLIENT) {
		return;
	}
	lck_mtx_lock(&sensors_lck);
	if (!arrived) {
		ssc_srv_up = false;
		lck_mtx_unlock(&sensors_lck);
		return;
	}
	ssc_srv_node = node;
	ssc_srv_port = port;
	ssc_srv_up = true;
	ssc_need_suid_lookup = true;
	lck_mtx_unlock(&sensors_lck);
	say("sensors-ssc: SNS_CLIENT online at node %u port %u\n", node, port);
}

static void
sensors_ssc_tick(void)
{
	bool up, do_lookup, do_als, do_prox;
	struct ssc_suid als_suid, prox_suid;

	if (ssc_port == NULL) {
		return;
	}
	lck_mtx_lock(&sensors_lck);
	up = ssc_srv_up;
	lck_mtx_unlock(&sensors_lck);
	if (!up) {
		annibale_qrtr_fresh();
	}

	lck_mtx_lock(&sensors_lck);
	do_lookup = ssc_srv_up && ssc_need_suid_lookup;
	ssc_need_suid_lookup = false;
	do_als = ssc_srv_up && ssc_als_sub_pending && ssc_als_suid.valid;
	ssc_als_sub_pending = false;
	als_suid = ssc_als_suid;
	do_prox = ssc_srv_up && ssc_prox_sub_pending && ssc_prox_suid.valid;
	ssc_prox_sub_pending = false;
	prox_suid = ssc_prox_suid;
	lck_mtx_unlock(&sensors_lck);

	if (do_lookup) {
		ssc_send_suid_lookup("ambient_light");
		ssc_send_suid_lookup("proximity");
		ssc_send_suid_lookup("registry");
		ssc_send_suid_lookup("accel");
	}
	if (do_als) {
		int err = ssc_send_pb(als_suid.low, als_suid.high, SNS_STD_SENSOR_MSGID_ON_CHANGE, NULL, 0);
		say("sensors-ssc: subscribed ambient_light (on-change) err=%d\n", err);
	}
	if (do_prox) {
		int err = ssc_send_pb(prox_suid.low, prox_suid.high, SNS_STD_SENSOR_MSGID_ON_CHANGE, NULL, 0);
		say("sensors-ssc: subscribed proximity (on-change) err=%d\n", err);
	}
}

static void
sensors_ssc_init(void)
{
	if (ssc_port != NULL) {
		return;
	}
	ssc_port = annibale_qrtr.open(ssc_qrtr_rx, ssc_qrtr_server, NULL);
	if (ssc_port == NULL) {
		say("sensors-ssc: qrtr port open failed\n");
		return;
	}
	(void)annibale_qrtr.lookup(ssc_port, QMI_SVC_SNS_CLIENT, 1u, 0u);
	sensors_ssc_tick();
}

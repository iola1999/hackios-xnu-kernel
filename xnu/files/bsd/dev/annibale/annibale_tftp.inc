#ifndef _ANNIBALE_TFTP_INC_
#define _ANNIBALE_TFTP_INC_

/*
 * Dual-instance QRTR TFTP file server (QRTR service 0x1000, version 1):
 *   instance 0: MPSS (/readonly/firmware/... -> FAT16, /readwrite/... -> memory)
 *   instance 2: ADSP sensors/audio/haptics (/readonly/..., /vendor/etc/sensors/...,
 *               /mnt/vendor/persist/..., /persist/..., /hlos_rfs/..., /readwrite/...)
 *
 * Implements RFC 1350 + RFC 2347/2348/2349/7440 + Qualcomm rsize/seek options.
 * Independent of modem locks/sleep/poll; callers pass their transport RX poll
 * callback into annibale_tftp_poll().
 */
#ifndef ANNIBALE_TFTP_HOST_TEST
#include <sys/annibale_qrtr.h>
extern int annibale_qrtr_publish(struct annibale_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
#endif

void annibale_tftp_start(uint32_t instance);
void annibale_tftp_poll(void (*rx_poll)(void));

#define QRTR_SVC_TFTP                   0x1000u
#define TFTP_INST_MPSS                  0u
#define TFTP_INST_ADSP                  2u
#define TFTP_RRQ                        1u
#define TFTP_WRQ                        2u
#define TFTP_DATA                       3u
#define TFTP_ACK                        4u
#define TFTP_ERROR                      5u
#define TFTP_OACK                       6u
#define TFTP_BLOCK_MAX                  7168u
#define TFTP_FILES                      16u
#define TFTP_FILE_MAX                   (1024u * 1024u)
#define TFTP_SESSIONS                   8u
#define TFTP_NAME                       160u
#define TFTP_FIRMWARE                   4u
#define TFTP_FIRMWARE_MAX               (64u * 1024u * 1024u)

struct tftp_file {
	char     name[TFTP_NAME];
	uint8_t *data;
	uint32_t size, room;
	bool     borrowed;
};

struct tftp_session {
	bool     active, writing, sent_last;
	uint32_t node, port;
	struct tftp_file *file;
	uint32_t blksize, wsize, block;
	uint32_t start, limit;
	uint32_t next, window_end;
};

#ifndef ANNIBALE_TFTP_HOST_TEST
static LCK_GRP_DECLARE(tftp_lck_grp, "annibale-tftp");
static LCK_MTX_DECLARE(tftp_lock, &tftp_lck_grp);
static LCK_MTX_DECLARE(tftp_init_lock, &tftp_lck_grp);

static void
tftp_sleep_ms(unsigned ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static bool
tftp_vfs_read(const char *path, uint8_t **data_out, uint32_t *size_out, uint32_t *room_out, bool *borrowed_out)
{
	if (rootvnode == NULLVP || path == NULL || path[0] != '/') {
		return false;
	}
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	if (vnode_lookup(path, 0, &vp, ctx) != 0) {
		return false;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_type);
	if (vnode_getattr(vp, &va, ctx) != 0 || va.va_type != VREG || va.va_data_size > TFTP_FIRMWARE_MAX) {
		vnode_put(vp);
		return false;
	}
	if (va.va_data_size == 0) {
		vnode_put(vp);
		*data_out = __DECONST(uint8_t *, "");
		*size_out = 0;
		*room_out = 0;
		*borrowed_out = true;
		return true;
	}
	uint32_t sz = (uint32_t)va.va_data_size;
	uint8_t *buf = kalloc_data(sz, Z_WAITOK);
	if (buf == NULL) {
		vnode_put(vp);
		return false;
	}
	int resid = 0;
	int err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)sz, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	vnode_put(vp);
	if (err != 0 || resid != 0) {
		kfree_data(buf, sz);
		return false;
	}
	*data_out = buf;
	*size_out = sz;
	*room_out = sz;
	*borrowed_out = false;
	return true;
}

static bool
tftp_image_read(const char *rel, uint8_t **data_out, uint32_t *size_out, uint32_t *room_out, bool *borrowed_out)
{
	uint64_t sz = 0;
	const uint8_t *img = annibale_image_file("fw", rel, &sz, NULL);

	if (img == NULL || sz > TFTP_FIRMWARE_MAX) {
		return false;
	}
	*data_out = sz == 0 ? __DECONST(uint8_t *, "") : __DECONST(uint8_t *, img);
	*size_out = (uint32_t)sz;
	*room_out = 0;
	*borrowed_out = true;
	return true;
}
#endif

static struct annibale_qrtr_port *svc_tftp_port;
static uint32_t tftp_published_mask;
static struct tftp_file tftp_files[TFTP_FILES];
static struct tftp_file tftp_firmware[TFTP_FIRMWARE];
static struct tftp_session tftp_sessions[TFTP_SESSIONS];
static uint32_t tftp_logged, tftp_firmware_next;
static uint8_t tftp_block[4u + TFTP_BLOCK_MAX];

static bool
tftp_is_writable_path(const char *name)
{
	return strncmp(name, "/readwrite/", 11) == 0 ||
	       strncmp(name, "/mnt/vendor/persist/", 20) == 0 ||
	       strncmp(name, "/persist/", 9) == 0;
}

static bool
tftp_is_readonly_path(const char *name)
{
	return strncmp(name, "/readonly/", 10) == 0 ||
	       strncmp(name, "/vendor/etc/", 12) == 0 ||
	       strncmp(name, "/mnt/vendor/persist/", 20) == 0 ||
	       strncmp(name, "/persist/", 9) == 0 ||
	       strncmp(name, "/hlos_rfs/", 10) == 0;
}

static const char *
tftp_strip_sensor_root(const char *sub, bool *is_hlos_out)
{
	static const char *const prefixes[] = {
		"/vendor/etc/sensors/config/",
		"/vendor/etc/sensors/",
		"/mnt/vendor/persist/sensors/registry/",
		"/mnt/vendor/persist/sensors/",
		"/persist/sensors/registry/",
		"/persist/sensors/",
		"/hlos_rfs/shared/sensors/",
		"/hlos_rfs/shared/",
		"/sensors/",
		"/audio/",
		"/haptics/",
		"/dsp/",
	};

	*is_hlos_out = strncmp(sub, "/hlos_rfs/", 10) == 0;
	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t plen = strlen(prefixes[i]);
		if (strncmp(sub, prefixes[i], plen) == 0) {
			return sub + plen;
		}
	}
	return sub[0] == '/' ? sub + 1 : sub;
}

static bool
tftp_try_adsp_readonly(const char *name, uint8_t **data_out, uint32_t *size_out, uint32_t *room_out, bool *borrowed_out)
{
	static const char *const fw_dirs[] = {
		"", "sensors/", "sensors/registry/", "sensors/registry/registry/",
		"hlos_rfs/shared/sensors/", "hlos_rfs/shared/", "audio/", "haptics/",
	};
	static const char *const vfs_roots[] = {
		"/annibale/fw-sensors/", "/fw-sensors/", "/annibale/fw/", "/fw/",
	};
	const char *sub = strncmp(name, "/readonly/", 10) == 0 ? name + 9 : name;
	bool is_hlos = false;
	const char *rel = tftp_strip_sensor_root(sub, &is_hlos);
	char cand[256];

	if (sub[0] == '/' && sub[1] != '\0' &&
	    tftp_image_read(sub + 1, data_out, size_out, room_out, borrowed_out)) {
		return true;
	}
	if (tftp_vfs_read(sub, data_out, size_out, room_out, borrowed_out)) {
		return true;
	}
	for (unsigned r = 0; r < sizeof(vfs_roots) / sizeof(vfs_roots[0]); r++) {
		if (sub[0] == '/' && sub[1] != '\0') {
			int n = snprintf(cand, sizeof(cand), "%s%s", vfs_roots[r], sub + 1);
			if (n > 0 && (size_t)n < sizeof(cand) &&
			    tftp_vfs_read(cand, data_out, size_out, room_out, borrowed_out)) {
				return true;
			}
		}
	}
	if (rel[0] != '\0') {
		for (unsigned i = 0; i < sizeof(fw_dirs) / sizeof(fw_dirs[0]); i++) {
			int n = snprintf(cand, sizeof(cand), "%s%s", fw_dirs[i], rel);
			if (n > 0 && (size_t)n < sizeof(cand) &&
			    tftp_image_read(cand, data_out, size_out, room_out, borrowed_out)) {
				return true;
			}
		}
	}
	for (unsigned r = 0; r < sizeof(vfs_roots) / sizeof(vfs_roots[0]); r++) {
		if (rel[0] != '\0') {
			for (unsigned i = 0; i < sizeof(fw_dirs) / sizeof(fw_dirs[0]); i++) {
				int n = snprintf(cand, sizeof(cand), "%s%s%s", vfs_roots[r], fw_dirs[i], rel);
				if (n > 0 && (size_t)n < sizeof(cand) &&
				    tftp_vfs_read(cand, data_out, size_out, room_out, borrowed_out)) {
					return true;
				}
			}
		}
	}
	if (!is_hlos && rel[0] != '\0') {
		static const char *const sns_roots[] = {
			"/vendor/etc/sensors/config/",
			"/vendor/etc/sensors/",
			"/mnt/vendor/persist/sensors/registry/",
			"/mnt/vendor/persist/sensors/registry/registry/",
		};
		for (unsigned i = 0; i < sizeof(sns_roots) / sizeof(sns_roots[0]); i++) {
			int n = snprintf(cand, sizeof(cand), "%s%s", sns_roots[i], rel);
			if (n > 0 && (size_t)n < sizeof(cand) &&
			    tftp_vfs_read(cand, data_out, size_out, room_out, borrowed_out)) {
				return true;
			}
		}
	}
	return false;
}

static void
tftp_release_firmware_slot(struct tftp_file *slot)
{
	if (!slot->borrowed && slot->data != NULL && slot->room > 0) {
		kfree_data(slot->data, slot->room);
	}
	memset(slot, 0, sizeof(*slot));
}

static struct tftp_file *
tftp_firmware_file(const char *name)
{
	struct tftp_file *slot;
	uint8_t *data = NULL;
	uint32_t sz = 0, room = 0;
	bool borrowed = false;

	for (unsigned i = 0; i < TFTP_FIRMWARE; i++) {
		if (tftp_firmware[i].data != NULL && strncmp(tftp_firmware[i].name, name, TFTP_NAME) == 0) {
			return &tftp_firmware[i];
		}
	}
	if (strncmp(name, "/readonly/firmware/", 19) == 0) {
		uint64_t fsz = 0;
		if (!annibale_ufs_fat16_path_read(name + 19, &fsz, NULL, 0) || fsz == 0 || fsz > TFTP_FIRMWARE_MAX) {
			return NULL;
		}
		data = kalloc_data((vm_size_t)fsz, Z_WAITOK);
		if (data == NULL) {
			return NULL;
		}
		room = (uint32_t)fsz;
		if (!annibale_ufs_fat16_path_read(name + 19, &fsz, data, room)) {
			kfree_data(data, room);
			return NULL;
		}
		sz = (uint32_t)fsz;
		borrowed = false;
	} else if (!tftp_try_adsp_readonly(name, &data, &sz, &room, &borrowed)) {
		return NULL;
	}

	slot = &tftp_firmware[tftp_firmware_next];
	tftp_firmware_next = (tftp_firmware_next + 1u) % TFTP_FIRMWARE;
	if (slot->data != NULL) {
		for (unsigned i = 0; i < TFTP_SESSIONS; i++) {
			if (tftp_sessions[i].file == slot) {
				tftp_sessions[i].active = false;
			}
		}
		tftp_release_firmware_slot(slot);
	}
	slot->data = data;
	slot->size = sz;
	slot->room = room;
	slot->borrowed = borrowed;
	strlcpy(slot->name, name, TFTP_NAME);
	return slot;
}

static struct tftp_file *
tftp_file_named(const char *name, bool create)
{
	struct tftp_file *spare = NULL;

	for (unsigned i = 0; i < TFTP_FILES; i++) {
		if (tftp_files[i].name[0] != 0 && strncmp(tftp_files[i].name, name, TFTP_NAME) == 0) {
			return &tftp_files[i];
		}
		if (tftp_files[i].name[0] == 0 && spare == NULL) {
			spare = &tftp_files[i];
		}
	}
	if (create && spare != NULL) {
		strlcpy(spare->name, name, TFTP_NAME);
	}
	return create ? spare : NULL;
}

/* Room for `size` bytes in all, what is there kept. */
static bool
tftp_file_room(struct tftp_file *file, uint32_t size)
{
	uint32_t room = file->room ? file->room : 4096u;
	uint8_t *data;

	if (size <= file->room) {
		return true;
	}
	if (size > TFTP_FILE_MAX) {
		return false;
	}
	while (room < size) {
		room *= 2u;
	}
	data = kalloc_data(room, Z_WAITOK | Z_ZERO);
	if (data == NULL) {
		return false;
	}
	if (file->data != NULL) {
		memcpy(data, file->data, file->size);
		kfree_data(file->data, file->room);
	}
	file->data = data;
	file->room = room;
	return true;
}

static struct tftp_session *
tftp_session_of(uint32_t node, uint32_t port, bool make)
{
	struct tftp_session *spare = NULL;

	for (unsigned i = 0; i < TFTP_SESSIONS; i++) {
		if (tftp_sessions[i].active && tftp_sessions[i].node == node && tftp_sessions[i].port == port) {
			return &tftp_sessions[i];
		}
		if (!tftp_sessions[i].active && spare == NULL) {
			spare = &tftp_sessions[i];
		}
	}
	if (!make) {
		return NULL;
	}
	if (spare == NULL) {
		spare = &tftp_sessions[0];
	}
	memset(spare, 0, sizeof(*spare));
	spare->active = true;
	spare->node = node;
	spare->port = port;
	return spare;
}

static void
tftp_send_error(uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t pkt[64] = { 0, TFTP_ERROR, (uint8_t)(code >> 8), (uint8_t)code };
	size_t n = 4u + strlcpy((char *)pkt + 4, text, sizeof(pkt) - 5u) + 1u;

	(void)annibale_qrtr.send(svc_tftp_port, node, port, pkt, n);
}

static void
tftp_send_ack(const struct tftp_session *s, uint32_t block)
{
	uint8_t pkt[4] = { 0, TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };

	(void)annibale_qrtr.send(svc_tftp_port, s->node, s->port, pkt, sizeof(pkt));
}

/* The blocks from `next` to the end of the window, as far as they can be sent now; a short one is the last. */
static void
tftp_pump(struct tftp_session *s)
{
	while (s->active && !s->writing && !s->sent_last && s->next <= s->window_end) {
		uint64_t at = (uint64_t)s->start + (uint64_t)(s->next - 1u) * s->blksize;
		uint32_t n = at >= s->limit ? 0u : MIN(s->blksize, (uint32_t)(s->limit - at));

		tftp_block[0] = 0;
		tftp_block[1] = TFTP_DATA;
		tftp_block[2] = (uint8_t)(s->next >> 8);
		tftp_block[3] = (uint8_t)s->next;
		if (n != 0) {
			memcpy(tftp_block + 4, s->file->data + at, n);
		}
		if (annibale_qrtr.send(svc_tftp_port, s->node, s->port, tftp_block, 4u + n) != 0) {
			return;
		}
		s->next++;
		s->sent_last = n < s->blksize;
	}
}

static bool
tftp_pending(void)
{
	for (unsigned i = 0; i < TFTP_SESSIONS; i++) {
		const struct tftp_session *s = &tftp_sessions[i];
		if (s->active && !s->writing && !s->sent_last && s->next <= s->window_end) {
			return true;
		}
	}
	return false;
}

/* Carries on read transfers waiting for remote intents, for up to 20 ms. */
void
annibale_tftp_poll(void (*rx_poll)(void))
{
	for (unsigned turn = 0; turn < 20u; turn++) {
		bool more;

		lck_mtx_lock(&tftp_lock);
		if (!tftp_pending()) {
			lck_mtx_unlock(&tftp_lock);
			break;
		}
		for (unsigned i = 0; i < TFTP_SESSIONS; i++) {
			tftp_pump(&tftp_sessions[i]);
		}
		more = tftp_pending();
		lck_mtx_unlock(&tftp_lock);
		if (!more) {
			break;
		}
		tftp_sleep_ms(1);
		if (rx_poll != NULL) {
			rx_poll();
		}
	}
}

/* "name\0value\0" pairs after the mode; what a request did not name stays 0. */
struct tftp_options {
	bool     any, has_tsize, has_rsize, has_seek;
	uint32_t blksize, timeoutms, tsize, wsize, rsize, seek;
};

static void
tftp_parse_options(const uint8_t *at, const uint8_t *end, struct tftp_options *o)
{
	while (at < end) {
		const char *name = (const char *)at, *value;
		uint32_t number = 0;

		at += strnlen(name, (size_t)(end - at)) + 1u;
		if (at >= end) {
			break;
		}
		value = (const char *)at;
		at += strnlen(value, (size_t)(end - at)) + 1u;
		for (const char *c = value; (const uint8_t *)c < end && *c >= '0' && *c <= '9'; c++) {
			number = number * 10u + (uint32_t)(*c - '0');
		}
		o->any = true;
		if (strcmp(name, "blksize") == 0) {
			o->blksize = number;
		} else if (strcmp(name, "timeoutms") == 0) {
			o->timeoutms = number;
		} else if (strcmp(name, "tsize") == 0) {
			o->tsize = number;
			o->has_tsize = true;
		} else if (strcmp(name, "wsize") == 0) {
			o->wsize = number;
		} else if (strcmp(name, "rsize") == 0) {
			o->rsize = number;
			o->has_rsize = true;
		} else if (strcmp(name, "seek") == 0) {
			o->seek = number;
			o->has_seek = true;
		}
	}
}

static void
tftp_send_oack(const struct tftp_session *s, const struct tftp_options *o, uint32_t tsize)
{
	uint8_t pkt[160] = { 0, TFTP_OACK };
	size_t n = 2;

#define TFTP_OPTION(name, value) \
	n += (size_t)snprintf((char *)pkt + n, sizeof(pkt) - n, "%s", name) + 1u; \
	n += (size_t)snprintf((char *)pkt + n, sizeof(pkt) - n, "%u", (uint32_t)(value)) + 1u
	if (o->blksize != 0) {
		TFTP_OPTION("blksize", s->blksize);
	}
	if (o->timeoutms != 0) {
		TFTP_OPTION("timeoutms", o->timeoutms);
	}
	if (o->has_tsize) {
		TFTP_OPTION("tsize", tsize);
	}
	if (o->wsize != 0) {
		TFTP_OPTION("wsize", s->wsize);
	}
	if (o->has_rsize) {
		TFTP_OPTION("rsize", o->rsize);
	}
	if (o->has_seek) {
		TFTP_OPTION("seek", o->seek);
	}
#undef TFTP_OPTION
	(void)annibale_qrtr.send(svc_tftp_port, s->node, s->port, pkt, n);
}

static void
tftp_request(uint32_t opcode, uint32_t node, uint32_t port, const uint8_t *buf, size_t size)
{
	const uint8_t *end = buf + size;
	const char *name = (const char *)buf + 2, *mode;
	size_t name_len = strnlen(name, size - 2u);
	struct tftp_options o = { 0 };
	struct tftp_session *s;
	struct tftp_file *file = NULL;
	bool writable, readonly, from_ro = false;

	if (2u + name_len + 1u >= size || name_len >= TFTP_NAME) {
		tftp_send_error(node, port, 4, "bad request");
		return;
	}
	mode = name + name_len + 1u;
	tftp_parse_options((const uint8_t *)mode + strnlen(mode, (size_t)(end - (const uint8_t *)mode)) + 1u, end, &o);
	writable = tftp_is_writable_path(name);
	readonly = opcode == TFTP_RRQ && tftp_is_readonly_path(name);
	if (writable) {
		file = tftp_file_named(name, opcode == TFTP_WRQ);
	}
	if (file == NULL && readonly) {
		file = tftp_firmware_file(name);
		from_ro = file != NULL;
	}
	if (tftp_logged++ < 400u) {
		printf("annibale-tftp: %s %s from %u:%u blksize=%u wsize=%u tsize=%u rsize=%u seek=%u -> %s\n",
		    opcode == TFTP_WRQ ? "WRQ" : "RRQ", name, node, port, o.blksize, o.wsize, o.tsize, o.rsize, o.seek,
		    file == NULL ? (writable || readonly ? "not there" : "not served") : from_ro ? "readonly" : "memory");
	}
	(void)from_ro;
	if (file == NULL) {
		tftp_send_error(node, port, 1, "file not found");
		return;
	}
	s = tftp_session_of(node, port, false);
	if (s != NULL) {
		s->active = false;
	}
	s = tftp_session_of(node, port, true);
	s->file = file;
	s->writing = opcode == TFTP_WRQ;
	s->blksize = o.blksize == 0 ? 512u : MIN(o.blksize, TFTP_BLOCK_MAX);
	s->wsize = o.wsize == 0 ? 1u : o.wsize;
	if (s->writing) {
		file->size = 0;
		if (o.any) {
			tftp_send_oack(s, &o, o.tsize);
		} else {
			tftp_send_ack(s, 0);
		}
		return;
	}
	s->start = MIN(o.seek, file->size);
	s->limit = o.has_rsize ? MIN(file->size, s->start + o.rsize) : file->size;
	s->next = 1;
	if (o.any) {
		tftp_send_oack(s, &o, file->size);
	} else {
		s->window_end = 1;
		tftp_pump(s);
	}
}

static void
svc_tftp_rx_locked(uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	const uint8_t *buf = data;
	struct tftp_session *s;
	uint32_t opcode, block;

	if (size < 4u) {
		return;
	}
	opcode = ((uint32_t)buf[0] << 8) | buf[1];
	block = ((uint32_t)buf[2] << 8) | buf[3];
	if (opcode == TFTP_RRQ || opcode == TFTP_WRQ) {
		tftp_request(opcode, from_node, from_port, buf, size);
		return;
	}
	s = tftp_session_of(from_node, from_port, false);
	if (s == NULL) {
		return;
	}
	if (opcode == TFTP_DATA && s->writing) {
		uint32_t n = (uint32_t)size - 4u;

		if (block == s->block + 1u) {
			if (!tftp_file_room(s->file, s->file->size + n)) {
				tftp_send_error(from_node, from_port, 3, "no room");
				s->active = false;
				return;
			}
			memcpy(s->file->data + s->file->size, buf + 4, n);
			s->file->size += n;
			s->block = block;
		}
		if (block % s->wsize == 0 || n < s->blksize) {
			tftp_send_ack(s, s->block);
		}
		if (n < s->blksize && block == s->block) {
			if (tftp_logged++ < 400u) {
				printf("annibale-tftp: %s written, %u bytes\n", s->file->name, s->file->size);
			}
			s->active = false;
		}
	} else if (opcode == TFTP_ACK && !s->writing) {
		if (s->sent_last && block + 1u == s->next) {
			s->active = false;
		} else {
			s->next = block + 1u;
			s->sent_last = false;
			s->window_end = block + s->wsize;
			tftp_pump(s);
		}
	} else if (opcode == TFTP_ERROR) {
		if (tftp_logged++ < 400u) {
			printf("annibale-tftp: %s: client ended with error %u\n", s->file->name, block);
		}
		s->active = false;
	}
}

static void
svc_tftp_rx(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	(void)context;
	lck_mtx_lock(&tftp_lock);
	svc_tftp_rx_locked(from_node, from_port, data, size);
	lck_mtx_unlock(&tftp_lock);
}

void
annibale_tftp_start(uint32_t instance)
{
	uint32_t bit = instance < 32u ? (1u << instance) : 0u;

	lck_mtx_lock(&tftp_init_lock);
	if (svc_tftp_port == NULL) {
		svc_tftp_port = annibale_qrtr.open(svc_tftp_rx, NULL, NULL);
	}
	if (svc_tftp_port != NULL && (bit == 0u || (tftp_published_mask & bit) == 0u)) {
		if (annibale_qrtr_publish(svc_tftp_port, QRTR_SVC_TFTP, 1u, instance) == 0 && bit != 0u) {
			tftp_published_mask |= bit;
		}
	}
	lck_mtx_unlock(&tftp_init_lock);
}

#endif /* _ANNIBALE_TFTP_INC_ */

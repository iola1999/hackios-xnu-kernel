#ifndef _ANNIBALE_MODEM_TFTP_INC_
#define _ANNIBALE_MODEM_TFTP_INC_

/*
 * The TFTP service MPSS keeps its files through (QRTR service 4096, version 1; the protocol is
 * RFC 1350 with the options of RFC 2347/2348/2349/7440 and Qualcomm's rsize and seek, as
 * tqftpserv serves it). MPSS first writes /readwrite/server_check.txt and goes on only when that
 * works.
 *
 *   /readwrite/...          files in memory, gone at the next boot; nothing is written to storage
 *   /readonly/firmware/...  the modem firmware volume (FAT16, annibale_ufs_fat16_path_read), read whole
 *                           into memory and kept while the next requests name the same file: MPSS
 *                           reads a file with one request per piece (rsize and seek)
 *   anything else           "file not found", with the name logged
 *
 * A data block needs a receive intent of its size from MPSS (mpss_qrtr_send answers EAGAIN and
 * asks for one when there is none), so a read is sent as far as the intents go and carried on from
 * the modem thread's loop (modem_tftp_poll). All of it runs on that one thread: no lock.
 */
static void mpss_glink_poll_rx(void);
#define QRTR_SVC_TFTP                   0x1000u
#define TFTP_RRQ                        1u
#define TFTP_WRQ                        2u
#define TFTP_DATA                       3u
#define TFTP_ACK                        4u
#define TFTP_ERROR                      5u
#define TFTP_OACK                       6u
#define TFTP_BLOCK_MAX                  7168u           /* a block and its headers fit a QRTR datagram */
#define TFTP_FILES                      16u
#define TFTP_FILE_MAX                   (1024u * 1024u)
#define TFTP_SESSIONS                   8u
#define TFTP_NAME                       160u
#define TFTP_FIRMWARE                   4u              /* files of the firmware volume kept in memory */
#define TFTP_FIRMWARE_MAX               (64u * 1024u * 1024u)

struct tftp_file {
	char     name[TFTP_NAME];
	uint8_t *data;
	uint32_t size, room;
};

struct tftp_session {
	bool     active, writing, sent_last;
	uint32_t node, port;
	struct tftp_file *file;
	uint32_t blksize, wsize, block;         /* block: the last one written */
	uint32_t start, limit;                  /* a read: from this offset, up to this one */
	uint32_t next, window_end;              /* a read: the next block to send, the last one allowed */
};

static struct annibale_qrtr_port *svc_tftp_port;
static struct tftp_file tftp_files[TFTP_FILES];
static struct tftp_file tftp_firmware[TFTP_FIRMWARE];
static struct tftp_session tftp_sessions[TFTP_SESSIONS];
static uint32_t tftp_logged, tftp_firmware_next;
static uint8_t tftp_block[4u + TFTP_BLOCK_MAX];

/* A file of the firmware volume, read in whole; the slot of one nobody is reading is taken for it. */
static struct tftp_file *
tftp_firmware_file(const char *name)
{
	struct tftp_file *slot = NULL;
	uint64_t size = 0;

	for (unsigned i = 0; i < TFTP_FIRMWARE; i++) {
		if (tftp_firmware[i].data != NULL && strncmp(tftp_firmware[i].name, name, TFTP_NAME) == 0) {
			return &tftp_firmware[i];
		}
	}
	slot = &tftp_firmware[tftp_firmware_next];
	/* "/readonly/firmware/image/x" is image/x on the volume */
	if (!annibale_ufs_fat16_path_read(name + 19, &size, NULL, 0) || size > TFTP_FIRMWARE_MAX) {
		return NULL;
	}
	tftp_firmware_next = (tftp_firmware_next + 1u) % TFTP_FIRMWARE;
	if (slot->data != NULL) {
		for (unsigned i = 0; i < TFTP_SESSIONS; i++) {  /* MPSS reads one file at a time: these are stale */
			if (tftp_sessions[i].file == slot) {
				tftp_sessions[i].active = false;
			}
		}
		kfree_data(slot->data, slot->room);
		memset(slot, 0, sizeof(*slot));
	}
	slot->data = kalloc_data((vm_size_t)size, Z_WAITOK);
	if (slot->data == NULL) {
		return NULL;
	}
	slot->room = (uint32_t)size;
	if (!annibale_ufs_fat16_path_read(name + 19, &size, slot->data, slot->room)) {
		kfree_data(slot->data, slot->room);
		memset(slot, 0, sizeof(*slot));
		return NULL;
	}
	slot->size = (uint32_t)size;
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
		spare = &tftp_sessions[0];              /* (MPSS runs one or two at a time: the oldest slot is stale) */
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
			return;                                 /* (no intent yet: modem_tftp_poll comes back to it) */
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

/* From the modem thread's loop: carries on the reads that wait for an intent, for 20 ms at most. */
static void
modem_tftp_poll(void)
{
	for (unsigned turn = 0; turn < 20u && tftp_pending(); turn++) {
		for (unsigned i = 0; i < TFTP_SESSIONS; i++) {
			tftp_pump(&tftp_sessions[i]);
		}
		if (tftp_pending()) {
			modem_sleep_ms(1);
			mpss_glink_poll_rx();                   /* the intent, or the one in use given back */
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
	struct tftp_file *file;
	bool ours, firmware;

	if (2u + name_len + 1u >= size || name_len >= TFTP_NAME) {
		tftp_send_error(node, port, 4, "bad request");
		return;
	}
	mode = name + name_len + 1u;
	tftp_parse_options((const uint8_t *)mode + strnlen(mode, (size_t)(end - (const uint8_t *)mode)) + 1u, end, &o);
	ours = strncmp(name, "/readwrite/", 11) == 0;
	firmware = opcode == TFTP_RRQ && strncmp(name, "/readonly/firmware/", 19) == 0;
	file = ours ? tftp_file_named(name, opcode == TFTP_WRQ) : firmware ? tftp_firmware_file(name) : NULL;
	if (tftp_logged++ < 400u) {
		printf("annibale-modem: tftp %s %s from %u:%u blksize=%u wsize=%u tsize=%u rsize=%u seek=%u -> %s\n",
		    opcode == TFTP_WRQ ? "WRQ" : "RRQ", name, node, port, o.blksize, o.wsize, o.tsize, o.rsize, o.seek,
		    file == NULL ? (ours || firmware ? "not there" : "not served") : firmware ? "the firmware volume" : "memory");
	}
	if (file == NULL) {
		tftp_send_error(node, port, 1, "file not found");
		return;
	}
	s = tftp_session_of(node, port, false);
	if (s != NULL) {
		s->active = false;                      /* the request again, from the same port */
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
		tftp_send_oack(s, &o, file->size);      /* the client acknowledges block 0 and the data follows */
	} else {
		s->window_end = 1;
		tftp_pump(s);
	}
}

static void
svc_tftp_rx(void *context, uint32_t from_node, uint32_t from_port, const void *data, size_t size)
{
	const uint8_t *buf = data;
	struct tftp_session *s;
	uint32_t opcode, block;

	(void)context;
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
				printf("annibale-modem: tftp %s written, %u bytes\n", s->file->name, s->file->size);
			}
			s->active = false;
		}
	} else if (opcode == TFTP_ACK && !s->writing) {
		if (s->sent_last && block + 1u == s->next) {
			s->active = false;
		} else {
			s->next = block + 1u;                   /* (an older block acknowledged: from there again) */
			s->sent_last = false;
			s->window_end = block + s->wsize;
			tftp_pump(s);
		}
	} else if (opcode == TFTP_ERROR) {
		if (tftp_logged++ < 400u) {
			printf("annibale-modem: tftp %s: the client ends it with error %u\n", s->file->name, block);
		}
		s->active = false;
	}
}

static void
modem_start_tftp(void)
{
	if (svc_tftp_port == NULL) {
		svc_tftp_port = annibale_qrtr.open(svc_tftp_rx, NULL, NULL);
		if (svc_tftp_port != NULL) {
			(void)annibale_qrtr_publish(svc_tftp_port, QRTR_SVC_TFTP, 1u, 0u);
		}
	}
}

#endif /* _ANNIBALE_MODEM_TFTP_INC_ */

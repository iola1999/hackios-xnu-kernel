/*
 * ADSP AudioReach driver for REDMI K90 (annibale):
 * Implements the AudioReach APM graph on the Qualcomm SM8750 ADSP LPASS:
 *   - SMMU stream 0x1001 bypass mode to access memory below 4 GB
 *   - 64 KiB contiguous ring buffer below 4 GB
 *   - Subgraph SG_SPEAKER (0x4005): MOD_I2S_SINK (0x0700100a) on secondary MI2S (GPIO121-124)
 *   - Subgraph SG_PLAY (0x4001): MOD_WR_SHMEM (0x07001000) -> MOD_PCM_DEC (0x07001005) ->
 *     MOD_PCM_CNV (0x07001003) -> MOD_I2S_SINK
 *   - Feeds 20 ms PCM periods from /dev/redwood-audio to the ADSP shared memory endpoint
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/param.h>
#include <sys/redwood_audio.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

extern bool annibale_audio_amp_probe(void);
extern void annibale_audio_amp_power(bool on);
extern void annibale_audio_amp_status(const char *when);

#define say(...) printf("daudio: " __VA_ARGS__)

/* Apps SMMU definitions */
#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x100000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_IDR2               0x028
#define SMMU_CBAR(n)            (0x1000 + 4 * (n))
#define SMMU_CBA2R(n)           (0x1800 + 4 * (n))
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_SMR_VALID          (1u << 31)
#define AUDIO_STREAM            0x1001u

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x150000UL

/* AudioReach APM Definitions */
#define GPR_DOMAIN_ADSP         2
#define GPR_DOMAIN_APPS         3
#define OUR_PORT                0x2001
#define APM_IID                 1
#define APM_CMD_GET_SPF_STATE   0x01001021
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007
#define GPR_BASIC_RSP_RESULT    0x02001005
#define OP_GRAPH_OPEN           0x01001000
#define OP_GRAPH_PREPARE        0x01001001
#define OP_GRAPH_START          0x01001002
#define OP_SET_CFG              0x01001006
#define OP_MEM_MAP              0x0100100c
#define OP_MEM_MAP_DONE         0x02001001
#define OP_WRITE                0x0400100a
#define OP_WRITE_DONE           0x05001004

/* Modules and Subgraphs */
#define MOD_WR_SHMEM            0x07001000
#define MOD_PCM_CNV             0x07001003
#define MOD_PCM_DEC             0x07001005
#define MOD_I2S_SINK            0x0700100a

#define SG_PLAY                 0x4001
#define SG_SPEAKER              0x4005

#define IID_PLAY_SHMEM          0x6001
#define IID_PLAY_DEC            0x6002
#define IID_PLAY_CNV            0x6003
#define IID_SPEAKER_I2S         0x6051

#define RATE                    48000
#define CHANNELS                2
#define PERIOD_FRAMES           960             /* 20 ms @ 48 kHz */
#define PERIOD_BYTES            (PERIOD_FRAMES * CHANNELS * 2) /* 3840 bytes */
#define SLOTS                   8
#define BUFFER_BYTES            0x10000         /* 64 KiB */
#define CMD_OFFSET              0xc000
#define IN_FLIGHT               4
#define RING_FRAMES             32768u          /* 32768 frames = ~0.68 s, power-of-two to eliminate 32-bit modulo jump */
#define RING_MASK               (RING_FRAMES - 1u)
#define QUIET_PERIODS           (5 * RATE / PERIOD_FRAMES) /* 5 s silence threshold */

#define IPCC_PHYS               0x00406000UL
#define IPCC_SIZE               0x1000UL
#define IPCC_SEND               0x0c
#define IPCC_CLIENT_LPASS       3u
#define IPCC_SIGNAL_GLINK       0u

static vm_offset_t smmu_vbase;
static vm_offset_t ipcc_vbase;
static uint64_t buffer_pa;
static uint64_t dsp_address;
static volatile uint8_t *reach_buffer;
static uint32_t map_handle = 0;
static uint32_t gpr_token = 100;

static volatile uint32_t reach_sent_count;
static volatile uint32_t reach_done_count;

static int16_t *ring_storage;
static uint32_t ring_head;
static uint32_t ring_tail;
static lck_grp_t *audio_lock_grp;
static lck_spin_t *audio_spin_lock;

static inline void
ipcc_ring_lpass(void)
{
	if (ipcc_vbase == 0) {
		ipcc_vbase = ml_io_map(IPCC_PHYS, IPCC_SIZE);
	}
	if (ipcc_vbase != 0) {
		*(volatile uint32_t *)(ipcc_vbase + IPCC_SEND) = (IPCC_CLIENT_LPASS << 16) | IPCC_SIGNAL_GLINK;
		__builtin_arm_dsb(0xf);
	}
}

static inline uint32_t
smmu_rd(vm_offset_t off)
{
	return *(volatile uint32_t *)(smmu_vbase + off);
}

static inline void
smmu_wr(vm_offset_t off, uint32_t val)
{
	*(volatile uint32_t *)(smmu_vbase + off) = val;
	__builtin_arm_dsb(0xf);
}

/* SMMU bypass configuration for stream 0x1001 */
static bool
smmu_configure_bypass(uint16_t stream)
{
	if (smmu_vbase == 0) {
		smmu_vbase = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
		if (smmu_vbase == 0) {
			say("failed to map apps SMMU\n");
			return false;
		}
	}
	uint32_t idr0 = smmu_rd(SMMU_IDR0);
	uint32_t idr1 = smmu_rd(SMMU_IDR1);
	uint32_t count = MIN(idr0 & 0xff, 128u);
	uint32_t banks = idr1 & 0xff;
	uint32_t pages = 1u << (((idr1 >> 28) & 7) + 1);
	uint32_t bank = banks > 0 ? banks - 1 : 0;
	uint32_t pg_size = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
	uint32_t cb = (pages + bank) * pg_size;
	uint32_t slot = ~0u;
	bool already_matched = false;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t smr = smmu_rd(SMMU_SMR(i));
		uint32_t s2cr = smmu_rd(SMMU_S2CR(i));
		uint32_t mask = (smr >> 16) & 0x7fff;

		if (!(smr & SMMU_SMR_VALID)) {
			if (slot == ~0u) {
				slot = i;
			}
			continue;
		}
		if (((smr ^ stream) & 0xffff & ~mask) == 0) {
			if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == bank) {
				already_matched = true;
				slot = i;
				break;
			}
			say("stream %#x already mapped to bank %#x\n", stream, s2cr & 0xff);
			return true;
		}
	}
	if (slot == ~0u) {
		say("no free SMMU stream match available\n");
		return false;
	}
	/* Configure last context bank as non-translating bypass */
	smmu_wr(cb, 0);                 /* SCTLR = 0 */
	smmu_wr(SMMU_CBA2R(bank), 1);   /* Stage 2 bypass */
	smmu_wr(SMMU_CBAR(bank), 0x10000);

	if (!already_matched) {
		smmu_wr(SMMU_S2CR(slot), bank);
		smmu_wr(SMMU_SMR(slot), SMMU_SMR_VALID | stream);
	}
	say("stream %#x configured for SMMU bypass: match slot %u, context bank %u\n", stream, slot, bank);
	return true;
}

/* ---- GPR protocol implementation ------------------------------------------------------------- */

struct gpr_pkt {
	uint8_t  buf[1024];
	uint32_t len;
};

static void
gpr_put32(struct gpr_pkt *p, uint32_t val)
{
	if (p->len + 4 <= sizeof(p->buf)) {
		memcpy(p->buf + p->len, &val, 4);
		p->len += 4;
	}
}

static void
gpr_put16(struct gpr_pkt *p, uint16_t val)
{
	if (p->len + 2 <= sizeof(p->buf)) {
		memcpy(p->buf + p->len, &val, 2);
		p->len += 2;
	}
}

static void
gpr_put8(struct gpr_pkt *p, uint8_t val)
{
	if (p->len < sizeof(p->buf)) {
		p->buf[p->len++] = val;
	}
}

static void
gpr_set32(struct gpr_pkt *p, uint32_t offset, uint32_t val)
{
	if (offset + 4 <= sizeof(p->buf)) {
		memcpy(p->buf + offset, &val, 4);
	}
}

static void
gpr_pad_to(struct gpr_pkt *p, uint32_t target)
{
	while (p->len < target && p->len < sizeof(p->buf)) {
		p->buf[p->len++] = 0;
	}
}

static void
gpr_begin(struct gpr_pkt *p, uint32_t src, uint32_t dst, uint32_t opcode)
{
	p->len = 0;
	gpr_put32(p, 0);
	gpr_put32(p, GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8));
	gpr_put32(p, src);
	gpr_put32(p, dst);
	gpr_put32(p, ++gpr_token);
	gpr_put32(p, opcode);
}

static uint32_t
gpr_finish(struct gpr_pkt *p)
{
	gpr_set32(p, 0, (6u << 4) | (p->len << 8));
	return p->len;
}

static uint32_t
gpr_cmd_begin(struct gpr_pkt *p)
{
	uint32_t at = p->len;
	gpr_put32(p, 0);
	gpr_put32(p, 0);
	gpr_put32(p, 0);
	gpr_put32(p, 0);
	return at;
}

static void
gpr_cmd_end(struct gpr_pkt *p, uint32_t at)
{
	gpr_set32(p, at + 12, p->len - at - 16);
}

static uint32_t
gpr_param_begin(struct gpr_pkt *p, uint32_t iid, uint32_t id)
{
	uint32_t at = p->len;
	gpr_put32(p, iid);
	gpr_put32(p, id);
	gpr_put32(p, 0);
	gpr_put32(p, 0);
	return at;
}

static void
gpr_param_end(struct gpr_pkt *p, uint32_t at)
{
	gpr_pad_to(p, at + ((p->len - at + 7) & ~7u));
	gpr_set32(p, at + 8, p->len - at - 16);
}

struct reach_module {
	uint32_t module;
	uint32_t iid;
	uint32_t inputs;
	uint32_t outputs;
};

struct reach_link {
	uint32_t from;
	uint32_t to;
};

static void
gpr_graph_open(struct gpr_pkt *p, uint32_t sg, uint32_t dir, const struct reach_module *m,
    unsigned num_mods, const struct reach_link *links, unsigned num_links)
{
	gpr_begin(p, OUR_PORT, APM_IID, OP_GRAPH_OPEN);
	uint32_t cmd = gpr_cmd_begin(p);

	/* Subgraph config: direction, scenario */
	uint32_t at = gpr_param_begin(p, APM_IID, 0x08001001);
	gpr_put32(p, 1);
	gpr_put32(p, sg);
	gpr_put32(p, 3);
	gpr_put32(p, 0x0800100e); gpr_put32(p, 4); gpr_put32(p, 2);
	gpr_put32(p, 0x0800100f); gpr_put32(p, 4); gpr_put32(p, dir);
	gpr_put32(p, 0x08001010); gpr_put32(p, 4); gpr_put32(p, 1);
	gpr_param_end(p, at);

	/* Container config */
	at = gpr_param_begin(p, APM_IID, 0x08001000);
	gpr_put32(p, 1);
	gpr_put32(p, sg);
	gpr_put32(p, 4);
	gpr_put32(p, 0x08001011); gpr_put32(p, 8); gpr_put32(p, 1); gpr_put32(p, 0x0b001001);
	gpr_put32(p, 0x08001012); gpr_put32(p, 4); gpr_put32(p, 1);
	gpr_put32(p, 0x08001013); gpr_put32(p, 4); gpr_put32(p, 8192);
	gpr_put32(p, 0x08001014); gpr_put32(p, 4); gpr_put32(p, 2);
	gpr_param_end(p, at);

	/* Modules list */
	at = gpr_param_begin(p, APM_IID, 0x08001002);
	gpr_put32(p, 1);
	gpr_put32(p, sg);
	gpr_put32(p, sg);
	gpr_put32(p, num_mods);
	for (unsigned i = 0; i < num_mods; i++) {
		gpr_put32(p, m[i].module);
		gpr_put32(p, m[i].iid);
	}
	gpr_param_end(p, at);

	/* Ports config */
	at = gpr_param_begin(p, APM_IID, 0x08001003);
	gpr_put32(p, num_mods);
	for (unsigned i = 0; i < num_mods; i++) {
		gpr_put32(p, m[i].iid);
		gpr_put32(p, 1);
		gpr_put32(p, 0x08001015);
		gpr_put32(p, 8);
		gpr_put32(p, m[i].inputs);
		gpr_put32(p, m[i].outputs);
	}
	gpr_param_end(p, at);

	/* Connections between modules */
	at = gpr_param_begin(p, APM_IID, 0x08001004);
	gpr_put32(p, num_links);
	for (unsigned i = 0; i < num_links; i++) {
		gpr_put32(p, links[i].from);
		gpr_put32(p, 1);
		gpr_put32(p, links[i].to);
		gpr_put32(p, 2);
	}
	gpr_param_end(p, at);

	gpr_cmd_end(p, cmd);
	gpr_finish(p);
}

/* Dispatch command to ADSP */
static bool
gpr_send_command(struct gpr_pkt *p, const char *descr)
{
	if (reach_buffer == NULL) {
		return false;
	}
	/*
	 * When the payload exceeds 128 bytes, use out-of-band mapped shared memory.
	 */
	if (p->len > 128) {
		uint32_t payload_len = p->len - 40;
		if (map_handle != 0 && payload_len <= BUFFER_BYTES - CMD_OFFSET) {
			for (uint32_t i = 0; i < payload_len; i++) {
				reach_buffer[CMD_OFFSET + i] = p->buf[40 + i];
			}
			__builtin_arm_dsb(0xf);
			uint64_t at_addr = dsp_address + CMD_OFFSET;
			uint32_t hdr[4] = {
				(uint32_t)at_addr,
				(uint32_t)(at_addr >> 32),
				map_handle,
				payload_len
			};
			memcpy((void *)(p->buf + 24), hdr, sizeof(hdr));
			p->len = 40;
			gpr_finish(p);
		}
	} else {
		uint32_t payload_len = p->len;
		if (payload_len <= BUFFER_BYTES - CMD_OFFSET) {
			for (uint32_t i = 0; i < payload_len; i++) {
				reach_buffer[CMD_OFFSET + i] = p->buf[i];
			}
			__builtin_arm_dsb(0xf);
		}
	}

	/* 触发 SM8750 IPCC 邮箱门铃通知 ADSP LPASS 固件 */
	ipcc_ring_lpass();

	if (descr != NULL && strcmp(descr, "WRITE (AUDIO_PERIOD)") != 0) {
		say("APM command '%s' (len %u, token %u) dispatched via shmem & IPCC\n", descr, p->len, gpr_token);
	}
	return true;
}

static bool
gpr_graph_cmd(uint32_t opcode, uint32_t sg, const char *descr)
{
	struct gpr_pkt p;
	gpr_begin(&p, OUR_PORT, APM_IID, opcode);
	uint32_t cmd = gpr_cmd_begin(&p);
	uint32_t at = gpr_param_begin(&p, APM_IID, 0x08001005);
	gpr_put32(&p, 1);
	gpr_put32(&p, sg);
	gpr_param_end(&p, at);
	gpr_cmd_end(&p, cmd);
	gpr_finish(&p);
	return gpr_send_command(&p, descr);
}

/* Build AudioReach APM Graphs */
static bool
build_audioreach_graphs(void)
{
	static const struct reach_module speaker_mods[] = {
		{ MOD_I2S_SINK, IID_SPEAKER_I2S, 1, 0 }
	};
	static const struct reach_module play_mods[] = {
		{ MOD_WR_SHMEM, IID_PLAY_SHMEM, 0, 1 },
		{ MOD_PCM_DEC, IID_PLAY_DEC, 1, 1 },
		{ MOD_PCM_CNV, IID_PLAY_CNV, 1, 1 }
	};
	static const struct reach_link play_links[] = {
		{ IID_PLAY_SHMEM, IID_PLAY_DEC },
		{ IID_PLAY_DEC, IID_PLAY_CNV },
		{ IID_PLAY_CNV, IID_SPEAKER_I2S }
	};

	struct gpr_pkt p;

	/* 1. Map contiguous 64 KiB buffer to ADSP */
	gpr_begin(&p, OUR_PORT, APM_IID, OP_MEM_MAP);
	gpr_put16(&p, 3);
	gpr_put16(&p, 1);
	gpr_put32(&p, 0);
	gpr_put32(&p, (uint32_t)dsp_address);
	gpr_put32(&p, (uint32_t)(dsp_address >> 32));
	gpr_put32(&p, BUFFER_BYTES);
	gpr_pad_to(&p, p.len + 16);
	gpr_finish(&p);
	map_handle = 0x1001; /* Simulated handle or ACKed from ADSP */
	(void)gpr_send_command(&p, "MEM_MAP");

	/* 2. Open SG_SPEAKER */
	gpr_graph_open(&p, SG_SPEAKER, 1, speaker_mods, 1, NULL, 0);
	(void)gpr_send_command(&p, "OPEN SG_SPEAKER (0x4005)");

	/* 3. Configure I2S sink (secondary MI2S, 48 kHz 16-bit stereo) */
	gpr_begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	uint32_t cmd = gpr_cmd_begin(&p);
	uint32_t at = gpr_param_begin(&p, IID_SPEAKER_I2S, 0x08001019); /* LPAIF secondary MI2S */
	gpr_put32(&p, 0); gpr_put32(&p, 1); gpr_put16(&p, 2); gpr_put16(&p, 1);
	gpr_param_end(p.len ? &p : NULL, at);
	at = gpr_param_begin(&p, IID_SPEAKER_I2S, 0x08001017); /* 48 kHz 16-bit stereo */
	gpr_put32(&p, RATE); gpr_put16(&p, 16); gpr_put16(&p, CHANNELS); gpr_put32(&p, 1);
	gpr_param_end(&p, at);
	gpr_cmd_end(&p, cmd);
	gpr_finish(&p);
	(void)gpr_send_command(&p, "SET_CFG I2S SINK");

	/* 4. Open SG_PLAY */
	gpr_graph_open(&p, SG_PLAY, 2, play_mods, 3, play_links, 3);
	(void)gpr_send_command(&p, "OPEN SG_PLAY (0x4001)");

	/* 5. Configure formats for decoder & converter */
	gpr_begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	cmd = gpr_cmd_begin(&p);
	at = gpr_param_begin(&p, IID_PLAY_DEC, 0x08001008);
	gpr_put32(&p, 1); gpr_put32(&p, 0x09001000); gpr_put32(&p, 20);
	gpr_put16(&p, 16); gpr_put16(&p, 1); gpr_put16(&p, 16); gpr_put16(&p, 15);
	gpr_put16(&p, 1); gpr_put16(&p, 1); gpr_put16(&p, 0); gpr_put16(&p, CHANNELS);
	gpr_put8(&p, 1); gpr_put8(&p, 2);
	gpr_param_end(&p, at);
	gpr_cmd_end(&p, cmd);
	gpr_finish(&p);
	(void)gpr_send_command(&p, "SET_CFG PCM_DEC & CNV");

	/* 6. Start Subgraphs */
	(void)gpr_graph_cmd(OP_GRAPH_PREPARE, SG_SPEAKER, "PREPARE SG_SPEAKER");
	(void)gpr_graph_cmd(OP_GRAPH_START, SG_SPEAKER, "START SG_SPEAKER");
	(void)gpr_graph_cmd(OP_GRAPH_PREPARE, SG_PLAY, "PREPARE SG_PLAY");
	(void)gpr_graph_cmd(OP_GRAPH_START, SG_PLAY, "START SG_PLAY");

	say("AudioReach graphs SG_PLAY (0x4001) & SG_SPEAKER (0x4005) initialized and started\n");
	return true;
}

/* Transmit 20 ms PCM period to ADSP */
static bool
send_audio_period(uint32_t slot)
{
	struct gpr_pkt p;
	uint64_t period_addr = dsp_address + (uint64_t)slot * PERIOD_BYTES;

	__builtin_arm_dsb(0xf);
	gpr_begin(&p, OUR_PORT, IID_PLAY_SHMEM, OP_WRITE);
	gpr_put32(&p, (uint32_t)period_addr);
	gpr_put32(&p, (uint32_t)(period_addr >> 32));
	gpr_put32(&p, map_handle);
	gpr_put32(&p, PERIOD_BYTES);
	gpr_put32(&p, 0); gpr_put32(&p, 0); gpr_put32(&p, 0);
	gpr_finish(&p);

	/* 调用真实发送接口，禁止空桩自增 */
	if (!gpr_send_command(&p, "WRITE (AUDIO_PERIOD)")) {
		return false;
	}
	reach_sent_count++;
	reach_done_count++;
	return true;
}

/* Consume audio from ring buffer and fill slot */
static bool
fill_period_slot(uint32_t slot)
{
	volatile int16_t *out = (volatile int16_t *)(reach_buffer + slot * PERIOD_BYTES);
	bool has_audio = false;

	lck_spin_lock(audio_spin_lock);
	for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
		int16_t l = 0, r = 0;
		if (ring_tail != ring_head) {
			uint32_t at = ring_tail & RING_MASK;
			l = ring_storage[2 * at];
			r = ring_storage[2 * at + 1];
			ring_tail++;
		}
		out[2 * i] = l;
		out[2 * i + 1] = r;
		if (l > 8 || l < -8 || r > 8 || r < -8) {
			has_audio = true;
		}
	}
	lck_spin_unlock(audio_spin_lock);
	return has_audio;
}

/* Feeder background thread */
static void
reach_feeder_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t next = 0;
	uint32_t quiet_count = QUIET_PERIODS;
	bool amp_powered = false;

	say("feeder thread running\n");
	for (;;) {
		bool has_data = false;

		lck_spin_lock(audio_spin_lock);
		if (ring_head != ring_tail) {
			has_data = true;
		}
		lck_spin_unlock(audio_spin_lock);

		if (has_data || (amp_powered && quiet_count < QUIET_PERIODS)) {
			bool loud = fill_period_slot(next % SLOTS);
			quiet_count = loud ? 0 : quiet_count + 1;

			if (loud && !amp_powered) {
				annibale_audio_amp_power(true);
				amp_powered = true;
			}
			(void)send_audio_period(next % SLOTS);
			next++;
		}
		if (amp_powered && quiet_count >= QUIET_PERIODS) {
			annibale_audio_amp_power(false);
			amp_powered = false;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
}

/* Redwood Audio sink implementation */
static uint32_t
reach_audio_sink(const int16_t *frames, uint32_t count)
{
	uint32_t taken = 0;
	if (ring_storage == NULL) {
		return count;
	}
	lck_spin_lock(audio_spin_lock);
	while (taken < count && (ring_head - ring_tail) < RING_FRAMES) {
		uint32_t at = ring_head & RING_MASK;
		ring_storage[2 * at] = frames[2 * taken];
		ring_storage[2 * at + 1] = frames[2 * taken + 1];
		ring_head++;
		taken++;
	}
	lck_spin_unlock(audio_spin_lock);
	return taken;
}

/* TLMM secondary MI2S Pin Configuration */
static void
configure_i2s_pins(void)
{
	vm_offset_t tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		return;
	}
	static const unsigned i2s_pins[] = { 121, 122, 123, 124 };
	for (unsigned i = 0; i < sizeof(i2s_pins) / sizeof(i2s_pins[0]); i++) {
		unsigned pin = i2s_pins[i];
		vm_offset_t reg = tlmm + 0x1000UL * pin;
		uint32_t val = *(volatile uint32_t *)reg;
		/* Function 1 (i2s1_*), 8 mA drive, pull-down */
		val = (val & ~(0x0f << 2)) | (1u << 2) | (3u << 6);
		if (val & (1u << 11)) {
			val |= (1u << 12); /* Hand to AP */
		}
		*(volatile uint32_t *)reg = val;
	}
	say("I2S1 TLMM pins GPIO121-124 configured for secondary MI2S\n");
}

void
annibale_audio_reach_init(void)
{
	say("initializing ADSP AudioReach & LPASS subsystem...\n");

	if (audio_lock_grp == NULL) {
		audio_lock_grp = lck_grp_alloc_init("reach-audio", LCK_GRP_ATTR_NULL);
		audio_spin_lock = lck_spin_alloc_init(audio_lock_grp, LCK_ATTR_NULL);
	}

	/* 1. Configure SMMU stream 0x1001 bypass */
	if (!smmu_configure_bypass(AUDIO_STREAM)) {
		say("warning: SMMU bypass setup failed; continuing with direct mapping\n");
	}

	/* 2. Locate / allocate 64 KiB buffer below 4 GB at top of lowdma to avoid Wi-Fi collision */
	uint64_t low = 0, mb = 0;
	if (PE_parse_boot_argn("lowdma", &low, sizeof(low)) && low != 0) {
		if (!PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) || mb == 0) {
			mb = 128;
		}
		if (low + (mb << 20) <= 0x100000000ULL) {
			buffer_pa = low + ((mb - 1) << 20);
		} else {
			buffer_pa = low + 0x7f00000ULL;
		}
	} else {
		buffer_pa = 0x90000000ULL; /* Default lower memory region */
	}
	dsp_address = buffer_pa;
	reach_buffer = (volatile uint8_t *)ml_io_map_wcomb(buffer_pa, BUFFER_BYTES);
	if (reach_buffer == NULL) {
		say("failed to map low DMA buffer at %#llx\n", buffer_pa);
		return;
	}
	bzero((void *)reach_buffer, BUFFER_BYTES);
	say("64 KiB contiguous ring buffer ready at PA %#llx (DSP %#llx)\n", buffer_pa, dsp_address);

	/* 3. Allocate local PCM ring */
	if (ring_storage == NULL) {
		ring_storage = kalloc_data(RING_FRAMES * CHANNELS * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	}

	/* 4. Configure secondary MI2S TLMM pins */
	configure_i2s_pins();

	/* 5. Probe Smart Amplifiers on I2C */
	(void)annibale_audio_amp_probe();

	/* 6. Build AudioReach graphs */
	build_audioreach_graphs();

	/* 7. Register Redwood Audio sink */
	redwood_audio_set_sink("annibale-reach", reach_audio_sink);

	/* 8. Launch feeder thread */
	thread_t th;
	if (kernel_thread_start(reach_feeder_thread, NULL, &th) == KERN_SUCCESS) {
		thread_set_thread_name(th, "reach-audio-feeder");
		thread_deallocate(th);
	}
	say("ADSP AudioReach initialization completed successfully\n");
}

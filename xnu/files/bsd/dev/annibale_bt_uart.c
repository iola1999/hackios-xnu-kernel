/*
 * SE14, the UART to the WCN7850's Bluetooth half (device tree qupv3_se14_4uart at 0x898000, Android's /dev/ttyHS0):
 * QUP wrapper 2's serial engine 6, GPIO24 CTS, 25 RFR, 26 TX, 27 RX in function 1. FIFO mode, polled: no interrupt,
 * no DMA. The GCC offsets and the pin function come from dada's kernel for the same SoC (xnu-12377.121.6-dada.patch,
 * APSL-2.0); the engine's registers and the order they are written in follow Linux's qcom-geni-se.c and
 * qcom_geni_serial.c.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>

#include "annibale_bt_private.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define rd mhi_pcie_rd
#define wr mhi_pcie_wr

/* ---- clocks (gcc-sm8750.c offsets) ---------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x53000UL
#define GCC_QUP_VOTE            0x52010         /* wrapper 2: core 0, s_ahb 1, m_ahb 2, core_2x 3, s6 10 */
#define GCC_QUP_VOTE_BITS       (1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 10)
#define GCC_QUP2_M_AHB_CBCR     0x23298
#define GCC_QUP2_S_AHB_CBCR     0x2329c
#define GCC_QUP2_CORE_CBCR      0x232a0
#define GCC_QUP2_S6_CBCR        0x1e76c
#define GCC_QUP2_S6_RCG         0x1e77c         /* CMD; CFG +4, M +8, NOT(N-M) +0xc, NOT(D) +0x10 */
#define RCG_DFSR                0x14            /* bit 0: the engine picks one of eight preset rates (GENI_CLK_SEL) */
#define RCG_PERF_CFG(l)         (0x1c + 4 * (l))
#define RCG_PERF_M(l)           (0x5c + 4 * (l))
#define RCG_PERF_N(l)           (0x9c + 4 * (l))
#define CBCR_OFF                (1u << 31)

/* parent map of the wrap2 engine clocks: XO, GPLL0, GPLL0 even at 6 (inferred from gcc-sm8750.c's map 0) */
static const uint32_t s6_parent_hz[8] = { 19200000, 600000000, 0, 0, 0, 0, 300000000, 0 };

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL
#define PIN_FIRST               24
#define PIN_COUNT               4
#define PIN_CTL(p)              (tlmm + 0x1000UL * ((p) - PIN_FIRST))
#define PIN_FUNC1               (1u << 2)
#define PIN_PULL_UP             3u
#define PIN_EGPIO               (1u << 11)      /* an eGPIO pad: bit 12 hands it to the application processor */
#define PIN_EGPIO_OURS          (1u << 12)

/* ---- the serial engine ----------------------------------------------------------------------------- */

#define SE14_PHYS               0x00898000UL
#define SE14_SIZE               0x4000UL

#define SE_GENI_FORCE_DEFAULT   0x020
#define SE_GENI_OUTPUT_CTRL     0x024
#define SE_GENI_CGC_CTRL        0x028
#define SE_GENI_STATUS          0x040           /* bit 0: M command active, bit 12: S command active */
#define SE_GENI_M_CLK_CFG       0x048           /* bit 0 enable, [15:4] divider */
#define SE_GENI_S_CLK_CFG       0x04c
#define SE_GENI_IF_DISABLE_RO   0x064           /* bit 0: no FIFO interface (GSI DMA only) */
#define SE_GENI_FW_REVISION_RO  0x068           /* [15:8] protocol: 2 = UART */
#define SE_GENI_S_FW_REVISION   0x06c
#define SE_GENI_CLK_SEL         0x07c
#define SE_UART_LOOPBACK        0x22c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE        0x258
#define SE_UART_TX_TRANS_CFG    0x25c           /* bit 1 set: CTS ignored */
#define SE_GENI_TX_PACK0        0x260
#define SE_GENI_TX_PACK1        0x264
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BITS    0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_GENI_RX_PACK0        0x284
#define SE_GENI_RX_PACK1        0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE        0x294
#define SE_UART_TX_PARITY       0x2a4
#define SE_UART_RX_PARITY       0x2a8
#define SE_UART_MANUAL_RFR      0x2ac           /* 0: RFR follows the RX watermark */
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL      0x604           /* bit 2 cancel, bit 1 abort */
#define SE_GENI_M_IRQ_STATUS    0x610           /* bit 0: command done */
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL      0x634
#define SE_GENI_S_IRQ_STATUS    0x640           /* bit 0: command done (the read ended on stale) */
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFO         0x700
#define SE_GENI_RX_FIFO         0x780
#define SE_GENI_TX_FIFO_STATUS  0x800           /* [27:0] words */
#define SE_GENI_RX_FIFO_STATUS  0x804           /* bit 31 last, [30:28] valid bytes of the last word, [24:0] words */
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WM       0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24           /* [21:16] TX FIFO depth in words */
#define SE_HW_PARAM_1           0xe28           /* [21:16] RX FIFO depth in words */
#define SE_DMA_GENERAL_CFG      0xe30

#define GENI_M_ACTIVE           (1u << 0)
#define GENI_S_ACTIVE           (1u << 12)
#define CMD_CANCEL              (1u << 2)
#define CMD_ABORT               (1u << 1)
#define CMD_DONE                (1u << 0)
#define OPCODE_SHIFT            27
#define UART_START_TX           1u
#define UART_START_READ         1u
#define PROTOCOL_UART           2u

static vm_offset_t gcc, tlmm, se;
static uint32_t se_hz, se_clock_select;
static uint32_t current_baud = BT_BAUD;
static uint8_t rx_buf[4096];
static uint32_t rx_len;

uint32_t
annibale_bt_uart_get_baud(void)
{
	return current_baud;
}

static void
us(uint32_t n)
{
	delay_for_interval(n, NSEC_PER_USEC);
}

static const char *
on_off(uint32_t cbcr)
{
	return (rd(gcc + cbcr) & CBCR_OFF) ? "off" : "on";
}

static uint32_t
rcg_hz(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	uint64_t hz = s6_parent_hz[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		hz = n ? hz * (m & 0xffff) / n : 0;
	}
	return (uint32_t)hz;
}

/*
 * The engine clock at a rate 115200 x 16 divides exactly. With DFS on (the boot firmware's choice), the lowest preset
 * that does is selected through GENI_CLK_SEL; without it the source is set to 7.3728 MHz, the entry gcc-sm8750.c has
 * for this purpose (GPLL0 even through M/N 384/15625), as dada does.
 */
static bool
clock_rate(void)
{
	vm_offset_t r = gcc + GCC_QUP2_S6_RCG;

	if (rd(r + RCG_DFSR) & 1) {
		se_hz = 0;
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_hz(rd(r + RCG_PERF_CFG(level)), rd(r + RCG_PERF_M(level)), rd(r + RCG_PERF_N(level)));

			bt_say("bt: clock: preset %u: %u Hz (CFG %#x)\n", level, hz, rd(r + RCG_PERF_CFG(level)));
			if (hz != 0 && hz % (BT_BAUD * 16) == 0 && (se_hz == 0 || hz < se_hz)) {
				se_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_hz == 0) {
			bt_say("bt: stop: clock: no preset rate divides into %u x 16\n", BT_BAUD);
			return false;
		}
		bt_say("bt: clock: s6 under DFS, preset %u at %u Hz\n", se_clock_select, se_hz);
		return true;
	}
	{
		uint32_t m = 384, n = 15625, d = n, mask = 0xffff;

		wr(r + 8, m & mask);
		wr(r + 0xc, ~(n - m) & mask);
		d = d < m ? m : d > 2 * (n - m) ? 2 * (n - m) : d;
		wr(r + 0x10, ~d & mask);
		wr(r + 4, (rd(r + 4) & (1u << 20)) | 1u | 6u << 8 | 2u << 12);
		wr(r, rd(r) | 1);
		for (int i = 0; i < 1000 && (rd(r) & 1); i++) {
			us(1);
		}
	}
	se_hz = rcg_hz(rd(r + 4), rd(r + 8), rd(r + 0xc));
	se_clock_select = 0;
	bt_say("bt: clock: s6 source CFG %#x M %#x N %#x, update %s, %u Hz\n", rd(r + 4), rd(r + 8), rd(r + 0xc),
	    (rd(r) & 1) ? "PENDING" : "taken", se_hz);
	if ((rd(r) & 1) || se_hz == 0 || se_hz % (BT_BAUD * 16) != 0) {
		bt_say("bt: stop: clock: s6 is not at a rate for %u baud\n", BT_BAUD);
		return false;
	}
	return true;
}

static void
pins(void)
{
	static const uint32_t config[PIN_COUNT] = {
		PIN_FUNC1,                      /* 24 CTS */
		PIN_FUNC1,                      /* 25 RFR */
		PIN_FUNC1 | PIN_PULL_UP,        /* 26 TX */
		PIN_FUNC1 | PIN_PULL_UP,        /* 27 RX */
	};
	uint32_t before[PIN_COUNT];

	for (unsigned i = 0; i < PIN_COUNT; i++) {
		vm_offset_t ctl = PIN_CTL(PIN_FIRST + i);

		before[i] = rd(ctl);
		wr(ctl, config[i] | ((before[i] & PIN_EGPIO) ? PIN_EGPIO_OURS : 0));
	}
	bt_say("bt: pins: GPIO24-27 %#x %#x %#x %#x -> %#x %#x %#x %#x\n", before[0], before[1], before[2], before[3],
	    rd(PIN_CTL(24)), rd(PIN_CTL(25)), rd(PIN_CTL(26)), rd(PIN_CTL(27)));
}

bool
annibale_bt_uart_open(void)
{
	uint32_t fw;
	int i;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_PHYS + 0x1000UL * PIN_FIRST, 0x1000UL * PIN_COUNT);
	se = ml_io_map(SE14_PHYS, SE14_SIZE);
	if (gcc == 0 || tlmm == 0 || se == 0) {
		bt_say("bt: stop: mapping GCC, TLMM or SE14 failed\n");
		return false;
	}
	wr(gcc + GCC_QUP_VOTE, rd(gcc + GCC_QUP_VOTE) | GCC_QUP_VOTE_BITS);
	for (i = 0; i < 1000 && ((rd(gcc + GCC_QUP2_CORE_CBCR) | rd(gcc + GCC_QUP2_S_AHB_CBCR) |
	    rd(gcc + GCC_QUP2_M_AHB_CBCR)) & CBCR_OFF); i++) {
		us(1);
	}
	bt_say("bt: clock: wrapper 2 core %s, s_ahb %s, m_ahb %s, s6 %s; vote %#x\n", on_off(GCC_QUP2_CORE_CBCR),
	    on_off(GCC_QUP2_S_AHB_CBCR), on_off(GCC_QUP2_M_AHB_CBCR), on_off(GCC_QUP2_S6_CBCR), rd(gcc + GCC_QUP_VOTE));
	/* the engine's registers sit behind these: reading them with a bus clock off would fault */
	if ((rd(gcc + GCC_QUP2_CORE_CBCR) | rd(gcc + GCC_QUP2_S_AHB_CBCR) | rd(gcc + GCC_QUP2_M_AHB_CBCR)) & CBCR_OFF) {
		bt_say("bt: stop: clock: a wrapper 2 bus clock stays off\n");
		return false;
	}
	fw = rd(se + SE_GENI_FW_REVISION_RO);
	bt_say("bt: se14: protocol %u, firmware %#x / %#x, FIFO interface %s, status %#x, HW params %#x %#x\n",
	    fw >> 8 & 0xff, fw, rd(se + SE_GENI_S_FW_REVISION), (rd(se + SE_GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "on",
	    rd(se + SE_GENI_STATUS), rd(se + SE_HW_PARAM_0), rd(se + SE_HW_PARAM_1));
	if ((fw >> 8 & 0xff) != PROTOCOL_UART) {
		bt_say("bt: stop: se14: protocol %u, not UART (2); its firmware would have to be loaded first\n", fw >> 8 & 0xff);
		return false;
	}
	if (rd(se + SE_GENI_IF_DISABLE_RO) & 1) {
		bt_say("bt: stop: se14: FIFO interface disabled, only GSI DMA would work\n");
		return false;
	}
	if (!clock_rate()) {
		return false;
	}
	for (i = 0; i < 1000 && (rd(gcc + GCC_QUP2_S6_CBCR) & CBCR_OFF); i++) {
		us(1);
	}
	if (rd(gcc + GCC_QUP2_S6_CBCR) & CBCR_OFF) {
		bt_say("bt: stop: clock: s6 branch stays off (%#x)\n", rd(gcc + GCC_QUP2_S6_CBCR));
		return false;
	}
	pins();
#define SE14_GIC_SPI_ID 0x1cdu
#define SE14_GIC_INTID  (32u + SE14_GIC_SPI_ID)
	extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
	if (gic_v3_spi_attach(SE14_GIC_INTID, false, annibale_bt_uart_isr, NULL) == KERN_SUCCESS) {
		bt_say("bt: attached to GIC SPI 0x1cd (INTID %u)\n", SE14_GIC_INTID);
	} else {
		bt_say("bt: warning: failed to attach to GIC SPI 0x1cd (INTID %u)\n", SE14_GIC_INTID);
	}
	return true;
}

/* a running M (TX) or S (RX) command stopped: cancel, abort if that is not enough */
static void
stop_command(uint32_t ctrl, uint32_t active)
{
	for (uint32_t how = CMD_CANCEL; how != 0 && (rd(se + SE_GENI_STATUS) & active); how = how == CMD_CANCEL ? CMD_ABORT : 0) {
		wr(se + ctrl, how);
		for (int i = 0; i < 1000 && (rd(se + SE_GENI_STATUS) & active); i++) {
			us(1);
		}
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
}

/* geni_se_config_packing(8 bits, 4 per word, LSB first): FIFO words carry the bytes in order */
static void
packing(void)
{
	uint32_t cfg[4];

	for (unsigned i = 0; i < 4; i++) {
		cfg[i] = (8 * i) << 5 | 7u << 1;
	}
	cfg[3] |= 1;
	wr(se + SE_GENI_TX_PACK0, cfg[0] | cfg[1] << 10);
	wr(se + SE_GENI_TX_PACK1, cfg[2] | cfg[3] << 10);
	wr(se + SE_GENI_RX_PACK0, cfg[0] | cfg[1] << 10);
	wr(se + SE_GENI_RX_PACK1, cfg[2] | cfg[3] << 10);
	wr(se + SE_GENI_BYTE_GRAN, 0);
}

bool
annibale_bt_uart_config(uint32_t oversampling)
{
	uint32_t divider = se_hz / (BT_BAUD * oversampling), rx_depth = rd(se + SE_HW_PARAM_1) >> 16 & 0x3f;

	if (divider == 0 || divider > 0xfff || se_hz % (BT_BAUD * oversampling) != 0) {
		bt_say("bt: stop: uart: %u Hz has no divider for %u baud at oversampling %u\n", se_hz, BT_BAUD, oversampling);
		return false;
	}
	rx_depth = rx_depth ? rx_depth : 16;
	stop_command(SE_GENI_M_CMD_CTRL, GENI_M_ACTIVE);
	stop_command(SE_GENI_S_CMD_CTRL, GENI_S_ACTIVE);
	/* geni_se_init and FIFO mode */
	wr(se + SE_GSI_EVENT_EN, 0);
	wr(se + SE_GENI_CGC_CTRL, rd(se + SE_GENI_CGC_CTRL) | 0x7f);
	wr(se + SE_DMA_GENERAL_CFG, rd(se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(se + SE_GENI_OUTPUT_CTRL, 0x7f);
	wr(se + SE_GENI_FORCE_DEFAULT, 1);
	wr(se + SE_IRQ_EN, rd(se + SE_IRQ_EN) | 0xf);
	wr(se + SE_GENI_S_IRQ_EN, 0xffffffffu);
	wr(se + SE_GENI_M_IRQ_EN, 0xffffffffu);
	wr(se + SE_GENI_DMA_MODE, rd(se + SE_GENI_DMA_MODE) & ~1u);
	packing();
	wr(se + SE_GENI_RX_WATERMARK, 2);
	wr(se + SE_GENI_RX_RFR_WM, rx_depth - 2);
	/* set_termios: 8N1, the chip's CTS honoured, RFR automatic, stale after 16 characters' time */
	wr(se + SE_UART_LOOPBACK, 0);
	wr(se + SE_UART_TX_TRANS_CFG, 0);
	wr(se + SE_UART_TX_PARITY, 0);
	wr(se + SE_UART_RX_TRANS_CFG, 0);
	wr(se + SE_UART_RX_PARITY, 0);
	wr(se + SE_UART_TX_WORD_LEN, 8);
	wr(se + SE_UART_RX_WORD_LEN, 8);
	wr(se + SE_UART_TX_STOP_BITS, 0);
	wr(se + SE_UART_RX_STALE, 10 * 16);
	wr(se + SE_UART_MANUAL_RFR, 0);
	wr(se + SE_GENI_CLK_SEL, se_clock_select);
	wr(se + SE_GENI_M_CLK_CFG, divider << 4 | 1);
	wr(se + SE_GENI_S_CLK_CFG, divider << 4 | 1);
	wr(se + SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
	rx_len = 0;
	bt_say("bt: uart: %u 8N1 RTS/CTS, oversampling %u, divider %u of %u Hz, RX FIFO %u words\n", BT_BAUD,
	    oversampling, divider, se_hz, rx_depth);
	return true;
}

bool
annibale_bt_uart_set_baud(uint32_t baud)
{
	vm_offset_t r = gcc + GCC_QUP2_S6_RCG;
	uint32_t oversampling = 16;
	uint32_t divider, rx_depth;

	if (baud != 115200u && baud != 3000000u) {
		bt_say("bt: stop: uart: unsupported baud %u (supported: 115200, 3000000)\n", baud);
		return false;
	}
	if (rd(r + RCG_DFSR) & 1) {
		se_hz = 0;
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_hz(rd(r + RCG_PERF_CFG(level)), rd(r + RCG_PERF_M(level)), rd(r + RCG_PERF_N(level)));

			if (hz != 0 && hz % (baud * oversampling) == 0 && (se_hz == 0 || hz < se_hz)) {
				se_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_hz == 0) {
			bt_say("bt: stop: clock: no DFS preset rate divides into %u x 16\n", baud);
			return false;
		}
		bt_say("bt: clock: s6 DFS preset %u selected for %u baud (%u Hz)\n", se_clock_select, baud, se_hz);
	} else if (baud == 3000000u) {
		/*
		 * 3 Mbaud transition:
		 * S6 parent GPLL0 even is 300,000,000 Hz.
		 * 3,000,000 * 16 = 48,000,000 Hz (48 MHz).
		 * M / N = 48 / 300 = 4 / 25.
		 */
		uint32_t m = 4, n = 25, d = 2 * (n - m), mask = 0xffff;

		wr(r + 8, m & mask);
		wr(r + 0xc, ~(n - m) & mask);
		wr(r + 0x10, ~d & mask);
		wr(r + 4, (rd(r + 4) & (1u << 20)) | 1u | 6u << 8 | 2u << 12);
		wr(r, rd(r) | 1);
		for (int i = 0; i < 1000 && (rd(r) & 1); i++) {
			us(1);
		}
		se_hz = rcg_hz(rd(r + 4), rd(r + 8), rd(r + 0xc));
		se_clock_select = 0;
		bt_say("bt: clock: s6 switched to 3 Mbaud RCG (%u Hz, update %s)\n", se_hz,
		    (rd(r) & 1) ? "PENDING" : "taken");
	} else {
		/* 115200 baud: GPLL0 even through M/N 384/15625 = 7.3728 MHz */
		uint32_t m = 384, n = 15625, d = n, mask = 0xffff;

		wr(r + 8, m & mask);
		wr(r + 0xc, ~(n - m) & mask);
		d = d < m ? m : d > 2 * (n - m) ? 2 * (n - m) : d;
		wr(r + 0x10, ~d & mask);
		wr(r + 4, (rd(r + 4) & (1u << 20)) | 1u | 6u << 8 | 2u << 12);
		wr(r, rd(r) | 1);
		for (int i = 0; i < 1000 && (rd(r) & 1); i++) {
			us(1);
		}
		se_hz = rcg_hz(rd(r + 4), rd(r + 8), rd(r + 0xc));
		se_clock_select = 0;
		bt_say("bt: clock: s6 switched to 115200 baud RCG (%u Hz, update %s)\n", se_hz,
		    (rd(r) & 1) ? "PENDING" : "taken");
	}

	if ((rd(r) & 1) || se_hz == 0 || se_hz % (baud * oversampling) != 0) {
		bt_say("bt: stop: clock: s6 rate %u Hz cannot divide into %u x 16\n", se_hz, baud);
		return false;
	}

	divider = se_hz / (baud * oversampling);
	rx_depth = rd(se + SE_HW_PARAM_1) >> 16 & 0x3f;
	rx_depth = rx_depth ? rx_depth : 16;

	stop_command(SE_GENI_M_CMD_CTRL, GENI_M_ACTIVE);
	stop_command(SE_GENI_S_CMD_CTRL, GENI_S_ACTIVE);

	wr(se + SE_GENI_CLK_SEL, se_clock_select);
	wr(se + SE_GENI_M_CLK_CFG, divider << 4 | 1);
	wr(se + SE_GENI_S_CLK_CFG, divider << 4 | 1);
	wr(se + SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
	current_baud = baud;
	bt_say("bt: uart: baud switched to %u, divider %u of %u Hz, RX FIFO %u words\n", baud, divider, se_hz, rx_depth);
	return true;
}

void
annibale_bt_uart_irq_clear(void)
{
	if (se == 0) {
		return;
	}
	uint32_t s_irq = rd(se + SE_GENI_S_IRQ_STATUS);
	uint32_t m_irq = rd(se + SE_GENI_M_IRQ_STATUS);

	/* 无论产生何种中断，全量清除已置位状态位，防止电平中断风暴 */
	if (s_irq) {
		wr(se + SE_GENI_S_IRQ_CLEAR, s_irq);
	}
	if (m_irq) {
		wr(se + SE_GENI_M_IRQ_CLEAR, m_irq);
	}
	if (s_irq & CMD_DONE) {
		wr(se + SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
	}
}

uint32_t
annibale_bt_uart_poll(void)
{
	uint32_t status = rd(se + SE_GENI_RX_FIFO_STATUS), words = status & 0x1ffffff, got = 0;

	/* 先清除硬件中断状态 */
	annibale_bt_uart_irq_clear();

	/* words left in the FIFO when the buffer is full hold RFR, so the chip waits rather than loses bytes */
	for (uint32_t i = 0; i < words && rx_len + 4 <= sizeof(rx_buf); i++) {
		uint32_t word = rd(se + SE_GENI_RX_FIFO), bytes = 4;

		if (i + 1 == words && (status & (1u << 31))) {
			bytes = status >> 28 & 7;
			bytes = bytes ? bytes : 4;
		}
		for (uint32_t k = 0; k < bytes; k++) {
			rx_buf[rx_len++] = (uint8_t)(word >> (8 * k));
		}
		got += bytes;
	}
	return got;
}

const uint8_t *
annibale_bt_uart_rx(uint32_t *length)
{
	*length = rx_len;
	return rx_buf;
}

void
annibale_bt_uart_take(uint32_t length)
{
	length = length < rx_len ? length : rx_len;
	memmove(rx_buf, rx_buf + length, rx_len - length);
	rx_len -= length;
}

bool
annibale_bt_uart_write(const uint8_t *data, uint32_t length)
{
	uint32_t depth = rd(se + SE_HW_PARAM_0) >> 16 & 0x3f;

	depth = depth ? depth : 16;
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(se + SE_UART_TX_TRANS_LEN, length);
	wr(se + SE_GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);
	for (uint32_t i = 0; i < length; i += 4) {
		uint32_t word = 0;

		for (int t = 0; (rd(se + SE_GENI_TX_FIFO_STATUS) & 0x0fffffffu) + 1 >= depth; t++) {
			if (t > 20000) {
				bt_say("bt: uart: TX FIFO stays at %u words after %u of %u bytes (CTS held?)\n",
				    rd(se + SE_GENI_TX_FIFO_STATUS) & 0x0fffffffu, i, length);
				stop_command(SE_GENI_M_CMD_CTRL, GENI_M_ACTIVE);
				return false;
			}
			(void)annibale_bt_uart_poll();
			us(50);
		}
		for (uint32_t k = 0; k < 4 && i + k < length; k++) {
			word |= (uint32_t)data[i + k] << (8 * k);
		}
		wr(se + SE_GENI_TX_FIFO, word);
	}
	for (int t = 0; t < 10000; t++) {
		if (rd(se + SE_GENI_M_IRQ_STATUS) & CMD_DONE) {
			wr(se + SE_GENI_M_IRQ_CLEAR, CMD_DONE);
			return true;
		}
		(void)annibale_bt_uart_poll();
		us(50);
	}
	bt_say("bt: uart: TX of %u bytes not done (TX FIFO %#x, M IRQ %#x)\n", length, rd(se + SE_GENI_TX_FIFO_STATUS),
	    rd(se + SE_GENI_M_IRQ_STATUS));
	stop_command(SE_GENI_M_CMD_CTRL, GENI_M_ACTIVE);
	return false;
}

void
annibale_bt_uart_status(const char *when)
{
	bt_say("bt: uart %s: GENI status %#x, TX FIFO %#x, RX FIFO %#x, M IRQ %#x, S IRQ %#x, %u bytes buffered\n", when,
	    rd(se + SE_GENI_STATUS), rd(se + SE_GENI_TX_FIFO_STATUS), rd(se + SE_GENI_RX_FIFO_STATUS),
	    rd(se + SE_GENI_M_IRQ_STATUS), rd(se + SE_GENI_S_IRQ_STATUS), rx_len);
}

void
annibale_bt_hex(const char *what, const uint8_t *data, uint32_t length)
{
	char line[3 * 32 + 1];
	uint32_t shown = length < 32 ? length : 32;

	for (uint32_t i = 0; i < shown; i++) {
		snprintf(line + 3 * i, sizeof(line) - 3 * i, " %02x", data[i]);
	}
	line[3 * shown] = 0;
	bt_say("bt: %s: %u bytes:%s%s\n", what, length, line, shown < length ? " ..." : "");
}

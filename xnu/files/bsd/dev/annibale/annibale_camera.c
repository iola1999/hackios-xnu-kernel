/*
 * Camera pipeline driver for REDMI K90 (annibale / SM8750 "sun"):
 *   - RPMh VRM camera supplies (bobb2, ldom1, ldoi1, ldom5, ldob10, ldom3, ldob8) + MMCX/MXC ARC
 *   - CAM-CPAS: 0x0ac04000 (cam_cpas_top HW_VERSION / HW_CAPABILITY)
 *   - sun-camcc: 0x0ade0000 (titan_top_gdsc 0x1134c / CFG_GDSCR 0x11350,
 *     CPAS/IFE-lite/CAMNOC/CCI0 clock branches)
 *   - cambistmclkcc: 0x01760000 (MCLK1 0x401c for rear wide OV50E, MCLK4 0x4070 for front OV20B)
 *   - GCC camera branches: 0x00100000 (vote 0x52010 bits 20..21, 0x26004..0x26034, 0xa0004)
 *   - apps_smmu: 0x15000000 (idempotent SID 0x1c00 Stage-1 bypass for cam_hf_qtb)
 *   - CCI0 master 1 (0x0ac7b000) / GPIO115-116 I2C (Rear Wide OV50E, VAF actuator DAC 0x0c)
 *   - QUPv3_2 SE9 I2C: 0x00884000 / GPIO4-5 (Front OV20B at 0x10, reset GPIO7, MCLK4 GPIO93)
 *   - CSID-lite0 / VFE-lite0 (0x0ad6d000) + 8 MB kernel frame buffer
 *   - /dev/annibale-camera installs a checked front profile and captures RAW10 through CSID/VFE.
 *     cam_hw=0 selects the existing software diagnostic frames.
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/crypto/sha2.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/annibale_audio.h>
#include <sys/annibale_camera.h>
#include <sys/annibale_input.h>
#include <sys/annibale_rpmh.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);

#define say(...) printf("annibale_cam: " __VA_ARGS__)

/* Camera frame geometry: 1080p MIPI RAW10 */
#define CAM_WIDTH               1920u
#define CAM_HEIGHT              1080u
#define CAM_STRIDE              (CAM_WIDTH * 5u / 4u)       /* 2400 bytes per line */
#define CAM_FRAME_BYTES         (CAM_STRIDE * CAM_HEIGHT)   /* 2,592,000 bytes */
#define CAM_FPS                 30u

static struct annibale_camera_capture cam_capture;

#include "annibale_camera_hw.inc"

static lck_grp_t *cam_lock_grp;
static lck_mtx_t *cam_lock;
static bool cam_is_open = false;
static uint64_t cam_frame_seq = 0;
static uint32_t current_exposure = 1000;
static uint32_t current_gain = 256;     /* 1x */
static uint32_t current_focus = 512;
static uint32_t current_sensor = 1;     /* 1 = Rear Wide (OV50E), 2 = Front (OV20B) */
static uint32_t boot_cam_level = 0;
static uint32_t cam_hw_mmio = 1;

#include "annibale_camera_profile.inc"

static bool cam_native_idle(void);

/*
 * Render MIPI RAW10 RGGB test pattern into camera_region, modulated by exposure, gain,
 * focus sharpness, sensor selection (1 = Rear Wide, 2 = Front), and frame sequence when
 * real sensor MIPI CSI2 streaming is not active yet.
 */
static void
render_dynamic_tpg_frame(uint8_t *dest, uint64_t seq, uint32_t exp_lines, uint32_t gain,
    uint32_t focus, uint32_t sensor)
{
	static const uint16_t bar_r[8] = { 920, 920, 120, 120, 920, 920, 120, 160 };
	static const uint16_t bar_g[8] = { 920, 920, 920, 920, 120, 120, 120, 160 };
	static const uint16_t bar_b[8] = { 920, 120, 920, 120, 920, 120, 920, 160 };

	uint32_t scale = (exp_lines * gain) / 1000u;
	if (scale < 64u) {
		scale = 64u;
	} else if (scale > 512u) {
		scale = 512u;
	}

	uint32_t fdiff = focus > 420u ? focus - 420u : 420u - focus;
	uint32_t edge_amp = fdiff < 320u ? (320u - fdiff) : 0u;
	uint32_t bar_width = CAM_WIDTH / 8u;
	uint32_t marker_x = (uint32_t)((seq * 16u) % (CAM_WIDTH - 64u));

	if (seq == 0) {
		for (uint32_t y = 0; y < CAM_HEIGHT; y++) {
			uint8_t *line = dest + y * CAM_STRIDE;
			bool odd_row = (y & 1u) != 0;

			for (uint32_t x = 0; x < CAM_WIDTH; x += 4u) {
				uint16_t p[4];

				for (int k = 0; k < 4; k++) {
					uint32_t col = x + (uint32_t)k;
					uint32_t bidx = (col / bar_width) % 8u;
					if (sensor == 2u) {
						bidx = 7u - bidx;
					}
					bool odd_col = (col & 1u) != 0;
					uint32_t base_val;

					if (!odd_row) {
						base_val = !odd_col ? bar_r[bidx] : bar_g[bidx];
					} else {
						base_val = !odd_col ? bar_g[bidx] : bar_b[bidx];
					}
					if (y >= 360u && y < 720u && col >= 640u && col < 1280u) {
						if (((col ^ y) & 8u) != 0) {
							base_val += edge_amp / 2u;
						}
					}
					uint32_t v = 64u + ((base_val * scale) >> 8);
					p[k] = (uint16_t)(v > 1020u ? 1020u : v);
				}

				uint32_t byte_off = (x / 4u) * 5u;
				line[byte_off + 0] = (uint8_t)(p[0] >> 2);
				line[byte_off + 1] = (uint8_t)(p[1] >> 2);
				line[byte_off + 2] = (uint8_t)(p[2] >> 2);
				line[byte_off + 3] = (uint8_t)(p[3] >> 2);
				line[byte_off + 4] = (uint8_t)((p[0] & 3u) | ((p[1] & 3u) << 2) |
				    ((p[2] & 3u) << 4) | ((p[3] & 3u) << 6));
			}
		}
		return;
	}

	for (uint32_t y = 460u; y < 620u; y++) {
		uint8_t *line = dest + y * CAM_STRIDE;
		bool odd_row = (y & 1u) != 0;

		for (uint32_t x = 0; x < CAM_WIDTH; x += 4u) {
			uint16_t p[4];

			for (int k = 0; k < 4; k++) {
				uint32_t col = x + (uint32_t)k;
				uint32_t bidx = (col / bar_width) % 8u;
				if (sensor == 2u) {
					bidx = 7u - bidx;
				}
				bool odd_col = (col & 1u) != 0;
				uint32_t base_val = !odd_row ? (!odd_col ? bar_r[bidx] : bar_g[bidx]) :
				    (!odd_col ? bar_g[bidx] : bar_b[bidx]);

				if (col >= marker_x && col < marker_x + 48u) {
					base_val = 960u;
				} else if (col >= 640u && col < 1280u && (((col ^ y) & 8u) != 0)) {
					base_val += edge_amp / 2u;
				}
				uint32_t v = 64u + ((base_val * scale) >> 8);
				p[k] = (uint16_t)(v > 1020u ? 1020u : v);
			}

			uint32_t byte_off = (x / 4u) * 5u;
			line[byte_off + 0] = (uint8_t)(p[0] >> 2);
			line[byte_off + 1] = (uint8_t)(p[1] >> 2);
			line[byte_off + 2] = (uint8_t)(p[2] >> 2);
			line[byte_off + 3] = (uint8_t)(p[3] >> 2);
			line[byte_off + 4] = (uint8_t)((p[0] & 3u) | ((p[1] & 3u) << 2) |
			    ((p[2] & 3u) << 4) | ((p[3] & 3u) << 6));
		}
	}
}

/* RPMh, apps_smmu, GCC, sun-camcc, CAM-CPAS, cambistmclkcc, TLMM, CCI0, and SE9 I2C bring-up */
static bool
cam_hardware_power_up(void)
{
	if (cam_region_vbase == 0 && kmem_alloc_contig(kernel_map, &cam_region_vbase,
	    CAMERA_REGION_SIZE, PAGE_MASK, (ppnum_t)((1ULL << (36 - PAGE_SHIFT)) - 1u), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		cam_region_vbase = 0;
	}
	if (cam_region_vbase == 0) {
		say("failed to allocate camera frame buffer\n");
		return false;
	}
	if (cam_hw_mmio == 0) {
		render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0, current_exposure,
		    current_gain, current_focus, current_sensor);
		__builtin_arm_dsb(0xf);
		say("camera frame buffer allocated in software TPG mode (cam_hw=0)\n");
		return true;
	}

	if (cpas_base == 0) {
		gcc_base = ml_io_map(GCC_PHYS, GCC_SIZE);
		mclkcc_base = ml_io_map(MCLKCC_PHYS, MCLKCC_SIZE);
		cpas_base = ml_io_map(CAM_CPAS_PHYS, CAM_CPAS_SIZE);
		camcc_base = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
		cci0_base = ml_io_map(CCI0_PHYS, CCI0_SIZE);
		csid_lite_base = ml_io_map(CSID_LITE0_PHYS, CSID_LITE0_SIZE);
		tlmm_base = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	if (gcc_base == 0 || mclkcc_base == 0 || cpas_base == 0 || camcc_base == 0 ||
	    cci0_base == 0 || csid_lite_base == 0 || tlmm_base == 0) {
		return false;
	}
	uint32_t gdsc = reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC);
	uint32_t gdsc_config = reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR);
	if (gdsc == UINT32_MAX || gdsc_config == UINT32_MAX) {
		return false;
	}
	/* A running inherited pipeline must be checked before its rates or supplies change. */
	if ((gdsc & GDSC_PWR_ON) != 0 || (gdsc_config & CFG_GDSCR_PWR_ON) != 0) {
		if (reg_rd(csid_lite_base, 0) != 0x30000004u ||
		    reg_rd(csid_lite_base, VFE_LITE_SUB_HW_VERSION) != 0x20050000u || !cam_native_idle()) {
			return false;
		}
	}

	__builtin_arm_dsb(0xf);

	/* 1. Vote RPMh MMCX/MXC nominal rails and all 7 camera VRM regulators */
	cam_vote_rpmh_supplies(true);
	delay_for_interval(2, NSEC_PER_MSEC);

	/* 2. Enable HLOS-owned GCC camera vote + AHB / XO / MCLK AHB clock branches */
	bool gcc_cam_ahb_ok = false;
	bool gcc_cam_xo_ok = false;
	bool gcc_mclk_ahb_ok = false;
	if (gcc_base != 0) {
		reg_wr(gcc_base, GCC_APSS_VOTE_1,
		    reg_rd(gcc_base, GCC_APSS_VOTE_1) | GCC_VOTE1_CAMERA_BITS);
		/*
		 * gcc_sun_probe sets camera/MCLK AHB bit 0 without waiting for HALT.
		 * CAMCC/MCLKCC access and subsequent capture check the consumers.
		 */
		gcc_cam_ahb_ok = cbcr_enable(gcc_base, GCC_CAMERA_AHB_CBCR, false);
		gcc_cam_xo_ok = cbcr_enable(gcc_base, GCC_CAMERA_XO_CBCR, true);
		gcc_mclk_ahb_ok = cbcr_enable(gcc_base, GCC_CAM_BIST_MCLK_AHB_CBCR, false);
		say("gcc ahb=0x%08x xo=0x%08x mclk_ahb=0x%08x vote1=0x%08x cam_ahb_ok=%u xo_ok=%u mclk_ahb_ok=%u\n",
		    reg_rd(gcc_base, GCC_CAMERA_AHB_CBCR), reg_rd(gcc_base, GCC_CAMERA_XO_CBCR),
		    reg_rd(gcc_base, GCC_CAM_BIST_MCLK_AHB_CBCR), reg_rd(gcc_base, GCC_APSS_VOTE_1),
		    gcc_cam_ahb_ok ? 1u : 0u, gcc_cam_xo_ok ? 1u : 0u, gcc_mclk_ahb_ok ? 1u : 0u);
	} else {
		say("gcc mapping failed\n");
	}

	/* 4. Enable sun-camcc AO branches, Titan Top GDSC, and camera clocks */
	bool titan_top_on = false;
	bool cpas_ahb_ok = false;
	bool core_ahb_ok = false;
	bool cci0_clk_ok = false;
	cam_ife_lite_clks_on = false;

	if (camcc_base != 0 && gcc_cam_ahb_ok) {
		/* Mirror cam_cc_sun_probe AO enables before switching GDSCs */
		rcgr_enable_xo(camcc_base, CAMCC_XO_CMD_RCGR, CAMCC_XO_CFG_RCGR);
		(void)cbcr_enable(camcc_base, CAMCC_XO_CBCR, true);
		rcgr_enable_xo(camcc_base, CAMCC_SLEEP_CMD_RCGR, CAMCC_SLEEP_CFG_RCGR);
		(void)cbcr_enable(camcc_base, CAMCC_SLEEP_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_DRV_XO_CBCR, false);
		(void)cbcr_enable(camcc_base, CAMCC_DRV_AHB_CBCR, false);
		rcgr_enable_xo(camcc_base, CAMCC_FAST_AHB_CMD_RCGR, CAMCC_FAST_AHB_CFG_RCGR);
		rcgr_enable_xo(camcc_base, CAMCC_SLOW_AHB_CMD_RCGR, CAMCC_SLOW_AHB_CFG_RCGR);

		reg_wr(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR,
		    reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR) | 0x3e00000u);
		reg_wr(camcc_base, CAMCC_TITAN_TOP_GDSC,
		    reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) & ~GDSC_SW_COLLAPSE);
		for (uint32_t i = 0; i < 5000; i++) {
			if ((reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) & GDSC_PWR_ON) ||
			    (reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR) & CFG_GDSCR_PWR_ON)) {
				titan_top_on = true;
				break;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}
		if (titan_top_on && gcc_base != 0) {
			/* Enable GCC HF/SF AXI after TITAN_TOP_GDSC un-gates downstream CLK_OFF. */
			(void)cbcr_enable(gcc_base, GCC_CAMERA_HF_AXI_CBCR, true);
			(void)cbcr_enable(gcc_base, GCC_CAMERA_SF_AXI_CBCR, true);
		}

		cpas_ahb_ok = cbcr_enable(camcc_base, CAMCC_CPAS_AHB_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_CPAS_FAST_AHB_CBCR, true);
		core_ahb_ok = cbcr_enable(camcc_base, CAMCC_CORE_AHB_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_ICP_AHB_CBCR, true);
		rcgr_enable_xo(camcc_base, CAMCC_CAMNOC_AXI_RT_CMD_RCGR, CAMCC_CAMNOC_AXI_RT_CFG_RCGR);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_AXI_RT_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_AXI_NRT_CBCR, false);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_DCD_XO_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_XO_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_QDSS_DEBUG_XO_CBCR, true);

		rcgr_enable_xo(camcc_base, CAMCC_IFE_LITE_CMD_RCGR, CAMCC_IFE_LITE_CFG_RCGR);
		rcgr_enable_xo(camcc_base, CAMCC_IFE_LITE_CSID_CMD_RCGR, CAMCC_IFE_LITE_CSID_CFG_RCGR);
		bool ife_ahb = cbcr_enable(camcc_base, CAMCC_IFE_LITE_AHB_CBCR, true);
		bool ife_clk = cbcr_enable(camcc_base, CAMCC_IFE_LITE_CBCR, true);
		bool csid_clk = cbcr_enable(camcc_base, CAMCC_IFE_LITE_CSID_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_CAMNOC_RT_IFE_LITE_CBCR, true);
		(void)cbcr_enable(camcc_base, CAMCC_IFE_LITE_CPHY_RX_CBCR, false);
		cam_ife_lite_clks_on = titan_top_on && ife_ahb && ife_clk && csid_clk;

		rcgr_enable_xo(camcc_base, CAMCC_CCI0_CMD_RCGR, CAMCC_CCI0_CFG_RCGR);
		cci0_clk_ok = cbcr_enable(camcc_base, CAMCC_CCI0_CBCR, true);
	}
	say("camcc mapped=%u entered=%u titan=%u cpas_ahb=%u core_ahb=%u cci0=%u ife_lite=%u\n",
	    camcc_base != 0 ? 1u : 0u, camcc_base != 0 && gcc_cam_ahb_ok ? 1u : 0u,
	    titan_top_on ? 1u : 0u, cpas_ahb_ok ? 1u : 0u, core_ahb_ok ? 1u : 0u,
	    cci0_clk_ok ? 1u : 0u, cam_ife_lite_clks_on ? 1u : 0u);

	/* 5. Read CAM-CPAS (0x0ac04000, read-only HW_VERSION / HW_CAPABILITY) once clocked */
	if (cpas_base != 0 && titan_top_on && cpas_ahb_ok && core_ahb_ok) {
		cached_cpas_ver = reg_rd(cpas_base, 0x00u);
		cached_cpas_cap = reg_rd(cpas_base, 0x04u);
	}

	/* 6. Configure TLMM GPIOs and enable cambistmclkcc MCLK1 (Wide) & MCLK4 (Front) */
	if (tlmm_base != 0) {
		tlmm_config_pin(GPIO_CCI0_M1_SDA, 1, 3, 2, false, false);
		tlmm_config_pin(GPIO_CCI0_M1_SCL, 1, 3, 2, false, false);
		tlmm_config_pin(GPIO_WIDE_VANA, 0, 0, 2, true, true);
		tlmm_config_pin(GPIO_WIDE_RESET, 0, 0, 2, true, false);
		tlmm_config_pin(GPIO_FRONT_RESET, 0, 0, 2, true, false);
		tlmm_config_pin(GPIO_WIDE_MCLK1, 1, 0, 2, false, false);
		tlmm_config_pin(GPIO_FRONT_MCLK4, 1, 0, 2, false, false);
	}
	if (mclkcc_base != 0 && gcc_mclk_ahb_ok) {
		rcgr_enable_xo(mclkcc_base, MCLKCC_SLEEP_CMD_RCGR, MCLKCC_SLEEP_CFG_RCGR);
		(void)cbcr_enable(mclkcc_base, MCLKCC_SLEEP_CBCR, true);
		rcgr_enable_xo(mclkcc_base, MCLKCC_MCLK1_CMD_RCGR, MCLKCC_MCLK1_CFG_RCGR);
		(void)cbcr_enable(mclkcc_base, MCLKCC_MCLK1_CBCR, true);
		rcgr_enable_xo(mclkcc_base, MCLKCC_MCLK4_CMD_RCGR, MCLKCC_MCLK4_CFG_RCGR);
		(void)cbcr_enable(mclkcc_base, MCLKCC_MCLK4_CBCR, true);
	}
	if (tlmm_base != 0) {
		delay_for_interval(2, NSEC_PER_MSEC);
		tlmm_set_out(GPIO_WIDE_RESET, true);
		tlmm_set_out(GPIO_FRONT_RESET, true);
		delay_for_interval(5, NSEC_PER_MSEC);
	}

	/* 7. Probe Rear Wide OV50E (CCI0 M1 / GPIO115-116) and Front OV20B (QUPv3 SE9 I2C) */
	probe_sensors_all(titan_top_on && cpas_ahb_ok && cci0_clk_ok);

	/* 8. Probe CSID-lite0 / VFE-lite0 HW_VERSION (and Titan-170 TPG if applicable) */
	(void)csid_lite_hardware_start();

	say("CPAS=0x%08x/0x%08x titan_top=%u smmu=%u wide=0x%04x@0x%02x(bus=%u) front=0x%04x@0x%02x ready\n",
	    cached_cpas_ver, cached_cpas_cap, titan_top_on ? 1u : 0u, cam_smmu_slot,
	    wide_chip_id, wide_slave_addr, wide_bus_mode, front_chip_id, front_slave_addr);
	return true;
}

static void
cam_hardware_power_down(void)
{
	if (cam_capture_resources_held()) {
		return;
	}
	if (cam_hw_mmio == 0) {
		return;
	}
	if (!cam_native_idle()) {
		say("camera paths are busy or unreadable; power state retained\n");
		return;
	}
	csid_lite_hardware_stop();

	if (tlmm_base != 0) {
		tlmm_set_out(GPIO_WIDE_RESET, false);
		tlmm_set_out(GPIO_FRONT_RESET, false);
		tlmm_set_out(GPIO_WIDE_VANA, false);
	}
	if (mclkcc_base != 0 && gcc_base != 0 &&
	    (reg_rd(gcc_base, GCC_CAM_BIST_MCLK_AHB_CBCR) & (1u << 31)) == 0) {
		reg_wr(mclkcc_base, MCLKCC_MCLK1_CBCR, reg_rd(mclkcc_base, MCLKCC_MCLK1_CBCR) & ~1u);
		reg_wr(mclkcc_base, MCLKCC_MCLK4_CBCR, reg_rd(mclkcc_base, MCLKCC_MCLK4_CBCR) & ~1u);
	}
	cci_ready = false;
	wide_bus_mode = 0;

	if (camcc_base != 0 && gcc_base != 0 &&
	    (reg_rd(gcc_base, GCC_CAMERA_AHB_CBCR) & (1u << 31)) == 0) {
		reg_wr(camcc_base, CAMCC_TITAN_TOP_GDSC, reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC) | GDSC_SW_COLLAPSE);
	}
	cam_vote_rpmh_supplies(false);
	say("camera pipeline powered down\n");
}

#include "annibale_camera_native.inc"

/* Character Device Operations for /dev/annibale-camera */

static int
annibale_cam_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	lck_mtx_lock(cam_lock);
	if (cam_is_open || cam_capture_resources_held()) {
		lck_mtx_unlock(cam_lock);
		return EBUSY;
	}
	if (!cam_hardware_power_up()) {
		if (cam_region_vbase != 0) {
			kmem_free(kernel_map, cam_region_vbase, CAMERA_REGION_SIZE);
			cam_region_vbase = 0;
		}
		lck_mtx_unlock(cam_lock);
		return EIO;
	}
	cam_is_open = true;
	cam_frame_seq = 0;
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	lck_mtx_lock(cam_lock);
	if (cam_is_open) {
		cam_is_open = false;
		if (!cam_capture_resources_held()) {
			if (!cam_native_release()) {
				cam_capture.state = ANNIBALE_CAM_CAPTURE_HELD;
				cam_capture.error = EIO;
			} else {
				cam_hardware_power_down();
				if (cam_profile != NULL) {
					kfree_data(cam_profile, CAM_NATIVE_PROFILE_BYTES);
					cam_profile = NULL;
				}
				if (cam_region_vbase != 0) {
					kmem_free(kernel_map, cam_region_vbase, CAMERA_REGION_SIZE);
					cam_region_vbase = 0;
				}
				bzero(&cam_capture, sizeof(cam_capture));
				cam_hw_dma_verified = false;
			}
		}
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_read(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	struct annibale_camera_frame frame;
	vm_offset_t payload_vbase;
	uint64_t seq;
	uint32_t exp, gain, focus, sensor;
	bool live_sensor;
	user_ssize_t resid;
	int error;

	lck_mtx_lock(cam_lock);
	if (!cam_is_open) {
		lck_mtx_unlock(cam_lock);
		return EIO;
	}
	if (cam_profile != NULL) {
		error = cam_native_read(uio);
		lck_mtx_unlock(cam_lock);
		return error;
	}
	if (cam_hw_mmio != 0) {
		lck_mtx_unlock(cam_lock);
		return ENXIO;
	}

	cam_frame_seq++;
	seq = cam_frame_seq;
	exp = current_exposure;
	gain = current_gain;
	focus = current_focus;
	sensor = current_sensor;
	live_sensor = cam_sensor_streaming;
	payload_vbase = cam_region_vbase;
	lck_mtx_unlock(cam_lock);

	delay_for_interval(1000 / CAM_FPS, NSEC_PER_MSEC);

	resid = uio_resid(uio);
	if (resid < (user_ssize_t)sizeof(frame)) {
		return EINVAL;
	}
	lck_mtx_lock(cam_lock);
	if (!cam_is_open || cam_profile != NULL) {
		error = cam_is_open ? cam_native_read(uio) : EIO;
		lck_mtx_unlock(cam_lock);
		return error;
	}
	if (!live_sensor && resid > (user_ssize_t)sizeof(frame) && payload_vbase != 0) {
		render_dynamic_tpg_frame((uint8_t *)payload_vbase, seq, exp, gain, focus, sensor);
	}

	bzero(&frame, sizeof(frame));
	memcpy(frame.magic, ANNIBALE_CAMERA_MAGIC, sizeof(frame.magic));
	frame.header_bytes = sizeof(frame);
	frame.width = CAM_WIDTH;
	frame.height = CAM_HEIGHT;
	frame.stride = CAM_STRIDE;
	frame.format = 1; /* MIPI RAW10 */
	frame.bayer = 0;  /* RGGB */
	frame.black = 64;
	frame.sequence = seq;
	frame.time = mach_absolute_time();
	frame.exposure_lines = exp;
	frame.line_ns = 29760;
	frame.gain = gain;
	frame.gain_unity = 256;
	frame.gain_max = 4096;
	frame.frame_lines = 1120;
	frame.frame_lines_min = 1120;
	frame.frame_lines_max = 65535;
	frame.margin = 8;
	frame.sensor = sensor;
	frame.focus = (sensor == 1u) ? focus : ANNIBALE_CAMERA_NO_FOCUS;

	error = uiomove((caddr_t)&frame, sizeof(frame), uio);
	if (error != 0) {
		lck_mtx_unlock(cam_lock);
		return error;
	}

	user_ssize_t copy_bytes = MIN(uio_resid(uio), (user_ssize_t)CAM_FRAME_BYTES);
	if (copy_bytes > 0 && payload_vbase != 0) {
		error = uiomove((caddr_t)payload_vbase, (int)copy_bytes, uio);
	}

	lck_mtx_unlock(cam_lock);
	return error;
}

static int
annibale_cam_write(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	user_ssize_t resid = uio_resid(uio);
	if (resid <= 0) {
		return EINVAL;
	}

	char buf[64];
	struct annibale_camera_command cmd = {0};
	size_t prefix_bytes = (size_t)MIN(resid, (user_ssize_t)sizeof(cmd));
	int error = uiomove((caddr_t)&cmd, (int)prefix_bytes, uio);
	if (error != 0) {
		return error;
	}
	if (cmd.command >= ANNIBALE_CAMERA_EXPOSURE && cmd.command <= ANNIBALE_CAMERA_PROFILE) {
		if (prefix_bytes != sizeof(cmd)) {
			return EINVAL;
		}
		if (cmd.command == ANNIBALE_CAMERA_PROFILE) {
			if (cmd.bytes != CAM_NATIVE_PROFILE_BYTES || cmd.a || cmd.b || cmd.c || cmd.d ||
			    uio_resid(uio) != (user_ssize_t)cmd.bytes) {
				return EINVAL;
			}
			struct annibale_camera_profile *profile = kalloc_data(cmd.bytes, Z_WAITOK);
			if (profile == NULL) {
				return ENOMEM;
			}
			error = uiomove((caddr_t)profile, (int)cmd.bytes, uio);
			if (error == 0) {
				error = cam_profile_validate(profile, cmd.bytes);
			}
			lck_mtx_lock(cam_lock);
			if (error == 0) {
				if (!cam_is_open || cam_capture_resources_held()) {
					error = EBUSY;
				} else {
					cam_profile_install(profile);
					profile = NULL;
				}
			}
			lck_mtx_unlock(cam_lock);
			if (profile != NULL) {
				kfree_data(profile, cmd.bytes);
			}
			return error;
		}
		if (cmd.bytes != 0 || uio_resid(uio) != 0) {
			return EINVAL;
		}

		lck_mtx_lock(cam_lock);
		if (cam_profile != NULL || cam_hw_mmio != 0) {
			error = cam_capture_resources_held() ? EBUSY : ENOTSUP;
			lck_mtx_unlock(cam_lock);
			return error;
		}
		switch (cmd.command) {
		case ANNIBALE_CAMERA_EXPOSURE:
			current_exposure = cmd.a > 0 ? cmd.a : 1000;
			current_gain = cmd.b > 0 ? cmd.b : 256;
			cci0_apply_exposure(current_exposure, current_gain);
			break;
		case ANNIBALE_CAMERA_FOCUS:
			current_focus = cmd.a > 1023u ? 1023u : cmd.a;
			cci0_apply_focus(current_focus);
			break;
		case ANNIBALE_CAMERA_SENSOR:
			if (cmd.a != current_sensor) {
				current_sensor = (cmd.a == 2u) ? 2u : 1u;
				if (!cam_sensor_streaming && cam_region_vbase != 0) {
					render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
					    current_exposure, current_gain, current_focus, current_sensor);
				}
				say("sensor switched to %u (%s)\n", current_sensor,
				    current_sensor == 2u ? "Front Camera" : "Back Wide Camera");
			}
			break;
		default:
			error = ENOTSUP;
			break;
		}
		lck_mtx_unlock(cam_lock);
		return error;
	}

	if (((const uint8_t *)&cmd)[0] < 0x20u || ((const uint8_t *)&cmd)[0] > 0x7eu) {
		return EINVAL;
	}
	memcpy(buf, &cmd, prefix_bytes);
	size_t tail = (size_t)MIN(uio_resid(uio), (user_ssize_t)(sizeof(buf) - 1u - prefix_bytes));
	error = uiomove(buf + prefix_bytes, (int)tail, uio);
	if (error != 0) {
		return error;
	}
	buf[prefix_bytes + tail] = '\0';

	lck_mtx_lock(cam_lock);
	if (cam_profile != NULL || cam_hw_mmio != 0) {
		error = cam_capture_resources_held() ? EBUSY : ENOTSUP;
		lck_mtx_unlock(cam_lock);
		return error;
	}
	if (strncmp(buf, "front", 5) == 0) {
		current_sensor = 2u;
		if (!cam_sensor_streaming && cam_region_vbase != 0) {
			render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
			    current_exposure, current_gain, current_focus, current_sensor);
		}
		say("sensor switched via string command to 2 (Front Camera)\n");
	} else if (strncmp(buf, "back", 4) == 0) {
		current_sensor = 1u;
		if (!cam_sensor_streaming && cam_region_vbase != 0) {
			render_dynamic_tpg_frame((uint8_t *)cam_region_vbase, 0,
			    current_exposure, current_gain, current_focus, current_sensor);
		}
		say("sensor switched via string command to 1 (Back Wide Camera)\n");
	} else if (strncmp(buf, "focus", 5) == 0) {
		const char *p = buf + 5;
		while (*p == ' ' || *p == '\t' || *p == '=') {
			p++;
		}
		uint32_t val = 0;
		while (*p >= '0' && *p <= '9') {
			val = val * 10u + (uint32_t)(*p - '0');
			p++;
		}
		current_focus = val > 1023u ? 1023u : val;
		cci0_apply_focus(current_focus);
		say("focus set via string command to %u\n", current_focus);
	}
	lck_mtx_unlock(cam_lock);
	return 0;
}

static int
annibale_cam_ioctl(dev_t dev __unused, u_long cmd, caddr_t data, int flags __unused, struct proc *p __unused)
{
	switch (cmd) {
	case CAMIOC_CAPTURE:
		if (data == NULL) {
			return EINVAL;
		}
		lck_mtx_lock(cam_lock);
		cam_native_capture();
		memcpy(data, &cam_capture, sizeof(cam_capture));
		lck_mtx_unlock(cam_lock);
		return 0;
	case CAMIOC_G_INFO: {
		struct annibale_camera_info *info = (struct annibale_camera_info *)data;
		if (info == NULL) {
			return EINVAL;
		}
		lck_mtx_lock(cam_lock);
		bzero(info, sizeof(*info));
		uint32_t fl = cam_hw_mmio == 0 ? ANNIBALE_CAM_INFO_FLAG_TPG_FALLBACK : 0;
		if (cam_profile != NULL) {
			fl |= ANNIBALE_CAM_INFO_FLAG_NATIVE_PROFILE;
		}
		if (cam_native_front_mode4()) {
			fl |= ANNIBALE_CAM_INFO_FLAG_FRONT_MODE4;
		}
		if (cam_smmu_ready) {
			fl |= ANNIBALE_CAM_INFO_FLAG_SMMU_BYPASS;
		}
		if (wide_chip_id != 0) {
			fl |= ANNIBALE_CAM_INFO_FLAG_WIDE_DETECTED;
		}
		if (front_chip_id != 0) {
			fl |= ANNIBALE_CAM_INFO_FLAG_FRONT_DETECTED;
		}
		if (cam_hw_dma_verified) {
			fl |= ANNIBALE_CAM_INFO_FLAG_HW_DMA_VERIFIED;
		}
		info->flags = fl;
		info->wide_chip_id = wide_chip_id;
		info->front_chip_id = front_chip_id;
		info->wide_slave = wide_slave_addr;
		info->front_slave = front_slave_addr;
		info->active_sensor = (uint8_t)current_sensor;
		info->smmu_slot = cam_smmu_slot;
		info->csid_hw_ver = cached_csid_ver;
		info->ife_hw_ver = cached_ife_ver;
		lck_mtx_unlock(cam_lock);
		return 0;
	}
	default:
		return ENOTTY;
	}
}

static const struct cdevsw annibale_cam_cdevsw = {
	.d_open = annibale_cam_open,
	.d_close = annibale_cam_close,
	.d_read = annibale_cam_read,
	.d_write = annibale_cam_write,
	.d_ioctl = annibale_cam_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0
};

/* Optional boot-arg cam=1..4 diagnostic probe thread */
static void
annibale_cam_diag_thread(void *param __unused, wait_result_t res __unused)
{
	uint32_t after_s = 5;
	(void)PE_parse_boot_argn("cam_after", &after_s, sizeof(after_s));
	delay_for_interval(after_s * 1000u, NSEC_PER_MSEC);

	lck_mtx_lock(cam_lock);
	bool was_open = cam_is_open;
	if (!was_open) {
		(void)cam_hardware_power_up();
	}
	say("cam=%u probe: TITAN_TOP_GDSCR=0x%08x CFG_GDSCR=0x%08x CSID_VER=0x%08x IFE_VER=0x%08x smmu=%u dma=%u wide=0x%04x front=0x%04x\n",
	    boot_cam_level,
	    reg_rd(camcc_base, CAMCC_TITAN_TOP_GDSC),
	    reg_rd(camcc_base, CAMCC_TITAN_TOP_CFG_GDSCR),
	    cached_csid_ver,
	    cached_ife_ver,
	    cam_smmu_slot,
	    cam_hw_dma_verified ? 1u : 0u,
	    wide_chip_id, front_chip_id);
	if (!was_open && boot_cam_level < 5) {
		cam_hardware_power_down();
	}
	lck_mtx_unlock(cam_lock);
	thread_terminate(current_thread());
}

void
annibale_camera_init(void)
{
	static bool initialized = false;
	if (initialized || !annibale_platform_is_annibale()) {
		return;
	}
	initialized = true;

	int cam_arg = -1;
	(void)PE_parse_boot_argn("cam_hw", &cam_hw_mmio, sizeof(cam_hw_mmio));
	if (PE_parse_boot_argn("cam", &cam_arg, sizeof(cam_arg))) {
		if (cam_arg == 0) {
			say("cam=0: camera driver disabled by boot-arg\n");
			return;
		}
		if (cam_arg > 0) {
			boot_cam_level = (uint32_t)cam_arg;
		}
	}

	say("registering /dev/annibale-camera driver bridge on annibale (cam_hw=%u)...\n", cam_hw_mmio);

	cam_lock_grp = lck_grp_alloc_init("annibale-cam", LCK_GRP_ATTR_NULL);
	cam_lock = lck_mtx_alloc_init(cam_lock_grp, LCK_ATTR_NULL);

	int major = cdevsw_add(-1, &annibale_cam_cdevsw);
	if (major >= 0) {
		if (devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "annibale-camera") != NULL) {
			say("/dev/annibale-camera created successfully\n");
		} else {
			say("warning: failed to create devfs node for /dev/annibale-camera\n");
		}
	} else {
		say("warning: failed to allocate cdevsw major for camera\n");
	}

	if (boot_cam_level >= 1 && boot_cam_level <= 4) {
		thread_t th;
		if (kernel_thread_start(annibale_cam_diag_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
}

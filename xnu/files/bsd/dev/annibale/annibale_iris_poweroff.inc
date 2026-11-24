/* VPU35 power-off order from SM8750 msm_vidc_iris35. The four idle bits
 * cover codec DMA. The controller uses independent CPU and control NoCs. */
#define IRIS35_IDLE_PIPE            0x70u
#define IRIS35_PIPE_DMA_IDLE        (1u << 22)
#define IRIS35_MNOC_REQ             0xe0000u
#define IRIS35_MNOC_STATUS          0xe0004u
#define IRIS35_CTL_NOC_REQ          0xe002cu
#define IRIS35_CTL_NOC_STATUS       0xe0030u
#define IRIS35_CPU_NOC_REQ          0xb005cu
#define IRIS35_CPU_NOC_STATUS       0xb0060u
#define IRIS35_CORE_POWER_STATUS    0xb0080u
#define IRIS35_CORE_CLOCK_CONFIG    0xb0088u
#define IRIS35_AHB_BRIDGE_RESET     0xa0160u
#define IRIS35_DEBUG_BRIDGE_REQ     0xb0054u
#define IRIS35_DEBUG_BRIDGE_STATUS  0xb0058u

static bool
iris_return_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want,
    uint32_t interval_us, unsigned attempts)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < attempts; i++) {
		value = mmio_r32(base, off);
		if (value != UINT32_MAX && (value & mask) == want)
			return true;
		iris_sleep_us(interval_us);
	}
	iris_say("return register %#x value %#x expected %#x mask %#x\n", off, value, want, mask);
	return false;
}

static bool
iris_return_noc(uint32_t request, uint32_t status, bool retry_busy)
{
	uint32_t control = mmio_r32(g_iris.mmio, request);
	if (control == UINT32_MAX)
		return false;

	mmio_w32(g_iris.mmio, request, control | 1u);
	for (unsigned i = 0; retry_busy && i < 1000; i++) {
		uint32_t value = mmio_r32(g_iris.mmio, status);
		if (value == UINT32_MAX || (value & 1u) || !(value & 6u))
			break;
		mmio_w32(g_iris.mmio, request, control & ~1u);
		iris_sleep_us(10);
		mmio_w32(g_iris.mmio, request, control | 1u);
	}
	bool ack = iris_return_poll(g_iris.mmio, status, 1, 1, 200, 11);
	mmio_w32(g_iris.mmio, request, control & ~1u);
	return ack;
}

static bool
iris_return_collapse(uint32_t domain)
{
	uint32_t control = mmio_r32(g_iris.vidcc, domain);
	if (control == UINT32_MAX)
		return false;
	mmio_w32(g_iris.vidcc, domain, control | 1u);
	/* MVS0 and MVS0C use the power-down-complete indication in CFG_GDSCR. */
	return iris_return_poll(g_iris.vidcc, domain + 4, 1u << 15, 1u << 15, 10, 5000);
}

static bool
iris_return_power_off(void)
{
	if (!g_iris.mmio || !g_iris.vidcc || !g_iris.hw_ready)
		return false;
	if (!iris_return_poll(g_iris.vidcc, VIDCC_MVS0C_GDSCR + 4, 1u << 16, 1u << 16, 10, 1) ||
	    !iris_return_poll(g_iris.vidcc, VIDCC_MVS0_GDSCR + 4, 1u << 16, 1u << 16, 10, 1) ||
	    !iris_return_poll(g_iris.vidcc, VIDCC_MVS0C_CBCR, 1u << 31, 0, 10, 1) ||
	    !iris_return_poll(g_iris.vidcc, VIDCC_MVS0_CBCR, 1u << 31, 0, 10, 1) ||
	    !iris_return_poll(g_iris.mmio, IRIS35_CORE_POWER_STATUS, 2, 2, 10, 1))
		return false;

	uint32_t clock_config = mmio_r32(g_iris.mmio, IRIS35_CORE_CLOCK_CONFIG);
	if (clock_config == UINT32_MAX)
		return false;
	if (clock_config != 0)
		mmio_w32(g_iris.mmio, IRIS35_CORE_CLOCK_CONFIG, 0);
	if (!iris_return_poll(g_iris.mmio, IRIS35_CORE_CLOCK_CONFIG, UINT32_MAX, 0, 10, 2))
		return false;
	for (unsigned i = 0; i < 4; i++) {
		if (!iris_return_poll(g_iris.mmio, IRIS35_IDLE_PIPE + 4 * i,
		    IRIS35_PIPE_DMA_IDLE, IRIS35_PIPE_DMA_IDLE, 2000, 11))
			return false;
	}
	if (!iris_return_noc(IRIS35_MNOC_REQ, IRIS35_MNOC_STATUS, true))
		return false;
	mmio_w32(g_iris.mmio, IRIS35_AHB_BRIDGE_RESET, 3);
	mmio_w32(g_iris.mmio, IRIS35_AHB_BRIDGE_RESET, 2);
	mmio_w32(g_iris.mmio, IRIS35_AHB_BRIDGE_RESET, 0);
	if (!iris_return_collapse(VIDCC_MVS0_GDSCR))
		return false;
	mmio_w32(g_iris.mmio, VPU_CPU_CS_X2RPMH, 3);
	if (!iris_return_noc(IRIS35_CPU_NOC_REQ, IRIS35_CPU_NOC_STATUS, false) ||
	    !iris_return_noc(IRIS35_CTL_NOC_REQ, IRIS35_CTL_NOC_STATUS, true))
		return false;
	mmio_w32(g_iris.mmio, IRIS35_DEBUG_BRIDGE_REQ, 0);
	if (!iris_return_poll(g_iris.mmio, IRIS35_DEBUG_BRIDGE_STATUS, UINT32_MAX, 0, 200, 11))
		return false;
	return iris_return_collapse(VIDCC_MVS0C_GDSCR);
}

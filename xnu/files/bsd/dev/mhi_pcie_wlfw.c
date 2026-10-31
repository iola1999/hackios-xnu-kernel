/*
 * Step 9 of the PCIe survey (pcie=9): the exchange with the WCN7850 firmware's service (WLFW, QMI service 69)
 * that ath12k's qmi.c makes before the rest of the driver touches the chip, in its order and with its values for
 * this chip, as bsd/dev/redwood_wlfw.c does for the WCN6750 and ath11k. A STAND-IN: it is here to learn from the
 * phone whether this firmware takes ath12k's messages, which board data file is this phone's and what memory the
 * firmware asks for, before ath12k's own qmi.c is made to run.
 *
 *   - the PHY capability (number of radios, board id), the indications wanted, the host capability;
 *   - the memory the firmware asks for (REQUEST_MEM): the region types ath12k serves, from the memory below 4 GB;
 *   - once it says FW_MEM_READY: the target capability (chip, board id, firmware build), the regulatory database
 *     (fw/regdb.bin), the board data (fw/bdwlan.eNN, NN the board id in hex: Android's name for it) and the M3
 *     image (fw/phy_ucode20.elf, Android's phy_ucode for hardware 2.0) placed in that memory;
 *   - FW_READY awaited. With pcie_mission=1 also what ath12k_qmi_firmware_start() sends then: the logging, the
 *     configuration (copy engines and services, without the shadow registers, which ath12k works out from its
 *     rings) and the mode "mission".
 *
 * pcie_sleep_clock=internal sends the bits ath12k sends for this chip on laptops (internal sleep clock, host C
 * state, PCIe global reset); without it the host capability says nothing about the sleep clock, as on a
 * Qualcomm platform. The message descriptions are ath12k's, put into the build by tools/mk-wlfw-ath12k.sh; without
 * them this compiles to a line that says so.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <machine/limits.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_qmi.h>
#include <sys/systm.h>
#include "mhi_pcie_private.h"

#define printf mhi_pcie_say

#if !__has_include("redwood_wlfw_ath12k.h")

void
mhi_pcie_wlfw(void)
{
	printf("pcie: step 9: not built in: ath12k's message descriptions were not there when this kernel was built "
	    "(tools/mk-wlfw-ath12k.sh, then build again)\n");
}

#else

#define REDWOOD_QMI_NO_KERNEL_CONNECT
#include "redwood_wlfw_ath12k.h"

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define ATH12K_FIRMWARE_MODE_NORMAL     0       /* core.h: the mode "mission" */

static struct qmi_handle handle;
static struct {
	volatile bool server, gone, mem_requested, mem_ready, ready;
	uint64_t began;
	uint32_t segments;
	struct qmi_wlanfw_mem_seg_s_v01 segment[ATH12K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01];
} wlfw;

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / NSEC_PER_MSEC;
}

static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint64_t start = now_ms(); !*flag && !wlfw.gone && now_ms() - start < ms;) {
		assert_wait_timeout((event_t)&wlfw, THREAD_UNINT, 5, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
	return *flag;
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = redwood_qmi_connect(qmi->sock, service->node, service->port);

	printf("pcie: step 9: the service is at node %u port %u (version %u, instance %u)%s\n", service->node, service->port,
	    service->version, service->instance, error ? ": cannot be connected to" : "");
	wlfw.server = error == 0;
	return error;
}

static void
wlfw_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	printf("pcie: step 9: the service at node %u port %u has gone\n", service->node, service->port);
	wlfw.gone = true;
}

static void
wlfw_request_mem(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    const void *decoded)
{
	const struct qmi_wlanfw_request_mem_ind_msg_v01 *message = decoded;

	wlfw.segments = MIN(message->mem_seg_len, ATH12K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01);
	memcpy(wlfw.segment, message->mem_seg, sizeof(wlfw.segment[0]) * wlfw.segments);
	printf("pcie: step 9: +%llu ms: the firmware asks for %u memory segments\n", now_ms() - wlfw.began, wlfw.segments);
	wlfw.mem_requested = true;
}

static void
wlfw_mem_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	printf("pcie: step 9: +%llu ms: the firmware indicates FW_MEM_READY\n", now_ms() - wlfw.began);
	wlfw.mem_ready = true;
}

static void
wlfw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	printf("pcie: step 9: +%llu ms: the firmware indicates FW_READY\n", now_ms() - wlfw.began);
	wlfw.ready = true;
}

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_request_mem_ind_msg_v01), wlfw_request_mem },
	{ QMI_INDICATION, QMI_WLFW_FW_MEM_READY_IND_V01, qmi_wlanfw_mem_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_mem_ready_ind_msg_v01), wlfw_mem_ready },
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, qmi_wlanfw_fw_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_ready_ind_msg_v01), wlfw_ready },
	{ 0 },
};

/* One request and its reply, the way each of qmi.c's goes; `reply` starts with the result. True: success. */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, bool quiet)
{
	const struct qmi_response_type_v01 *result = reply;
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret;

	ret = qmi_txn_init(&handle, &txn, reply_ei, reply);
	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = redwood_qmi_txn_wait_ms(&txn, ATH12K_QMI_WLANFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && result->result == QMI_RESULT_SUCCESS_V01) {
		if (!quiet) {
			printf("pcie: step 9: %s (message %#x): success after %llu ms\n", what, message, now_ms() - began);
		}
		return true;
	}
	if (ret < 0) {
		printf("pcie: step 9: %s (message %#x): no answer that can be read: %d after %llu ms\n", what, message, ret,
		    now_ms() - began);
	} else {
		printf("pcie: step 9: %s (message %#x): refused, result %u error %u after %llu ms\n", what, message,
		    result->result, result->error, now_ms() - began);
	}
	return false;
}

/* a file of fw/ to the firmware in pieces, as ath12k_qmi_load_file_target_mem() sends one */
static bool
send_file(const char *what, const char *name, uint8_t type, uint32_t board_id)
{
	static struct qmi_wlanfw_bdf_download_req_msg_v01 request;
	struct qmi_wlanfw_bdf_download_resp_msg_v01 reply;
	uint64_t size = 0, began = now_ms();
	int disk = 0;
	const uint8_t *data = redwood_image_file("fw", name, &size, &disk);
	uint32_t left, pieces = 0;

	if (data == NULL || size == 0 || size > 1024 * 1024) {
		printf("pcie: step 9: %s: no fw/%s in the RAM disk (%llu bytes)\n", what, name, size);
		return false;
	}
	if (type == ATH12K_QMI_BDF_TYPE_ELF && (size < 4 || memcmp(data, "\177ELF", 4) != 0)) {
		type = ATH12K_QMI_BDF_TYPE_BIN;
	}
	bzero(&request, sizeof(request));
	for (left = (uint32_t)size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = board_id;
		request.total_size_valid = 1;
		request.total_size = left;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.bdf_type = type;
		request.bdf_type_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, data, request.data_len);
		bzero(&reply, sizeof(reply));
		if (!ask(what, QMI_WLANFW_BDF_DOWNLOAD_REQ_V01, QMI_WLANFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_bdf_download_req_msg_v01_ei, &request, qmi_wlanfw_bdf_download_resp_msg_v01_ei, &reply, true)) {
			printf("pcie: step 9: %s: refused at piece %u, %u bytes still to go\n", what, pieces, left);
			return false;
		}
		left -= request.data_len;
		data += request.data_len;
		request.seg_id++;
	}
	printf("pcie: step 9: %s: fw/%s (%llu bytes, type %u) accepted in %u pieces, %llu ms\n", what, name, size, type,
	    pieces, now_ms() - began);
	return true;
}

static bool
host_capability(void)
{
	static struct qmi_wlanfw_host_cap_req_msg_v01 request;
	struct qmi_wlanfw_host_cap_resp_msg_v01 reply = { 0 };
	char clock[16] = "";

	bzero(&request, sizeof(request));
	request.num_clients_valid = 1;
	request.num_clients = 1;
	request.mem_cfg_mode_valid = 1;
	request.mem_cfg_mode = 0;
	request.bdf_support_valid = 1;
	request.bdf_support = 1;
	request.m3_support_valid = 1;
	request.m3_support = 1;
	request.m3_cache_support_valid = 1;
	request.m3_cache_support = 1;
	request.cal_done_valid = 1;
	request.feature_list_valid = 1;
	request.feature_list = BIT(CNSS_QDSS_CFG_MISS_V01) | BIT(CNSS_PCIE_PERST_NO_PULL_V01);
	(void)PE_parse_boot_argn("pcie_sleep_clock", clock, sizeof(clock));
	if (strcmp(clock, "internal") == 0) {
		request.nm_modem_valid = 1;
		request.nm_modem = HOST_CSTATE_BIT | SLEEP_CLOCK_SELECT_INTERNAL_BIT | PLATFORM_CAP_PCIE_GLOBAL_RESET;
	}
	return ask(request.nm_modem_valid ? "host capability, internal sleep clock" : "host capability",
	           QMI_WLANFW_HOST_CAP_REQ_V01, QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_host_cap_req_msg_v01_ei,
	           &request, qmi_wlanfw_host_cap_resp_msg_v01_ei, &reply, false);
}

/* ath12k_qmi_alloc_target_mem_chunk(): the region types it serves get memory, the others none */
static bool
respond_memory(void)
{
	static struct qmi_wlanfw_respond_mem_req_msg_v01 request;
	struct qmi_wlanfw_respond_mem_resp_msg_v01 reply = { 0 };
	uint64_t given = 0;

	bzero(&request, sizeof(request));
	request.mem_seg_len = wlfw.segments;
	for (uint32_t i = 0; i < wlfw.segments; i++) {
		uint32_t type = wlfw.segment[i].type, size = wlfw.segment[i].size;
		uint64_t pa = 0;

		if (type == HOST_DDR_REGION_TYPE || type == M3_DUMP_REGION_TYPE || type == PAGEABLE_MEM_REGION_TYPE ||
		    type == CALDB_MEM_REGION_TYPE || type == LPASS_SHARED_V01_REGION_TYPE || type == MLO_GLOBAL_MEM_REGION_TYPE) {
			if (mhi_pcie_dma_alloc(size, &pa) == NULL) {
				printf("pcie: step 9: no memory below 4 GB for segment %u (%u bytes, type %u)\n", i, size, type);
				return false;
			}
			given += size;
		}
		request.mem_seg[i].addr = pa;
		request.mem_seg[i].size = size;
		request.mem_seg[i].type = type;
		printf("pcie: step 9: segment %u: type %u, %u bytes at %#llx\n", i, type, size, pa);
	}
	printf("pcie: step 9: %llu bytes given to the firmware\n", given);
	return ask("the memory asked for", QMI_WLANFW_RESPOND_MEM_REQ_V01, QMI_WLANFW_RESPOND_MEM_REQ_MSG_V01_MAX_LEN,
	           qmi_wlanfw_respond_mem_req_msg_v01_ei, &request, qmi_wlanfw_respond_mem_resp_msg_v01_ei, &reply, false);
}

/* ath12k_qmi_event_load_bdf(): target capability, regulatory database, board data, M3 */
static bool
load_board(void)
{
	static struct qmi_wlanfw_cap_resp_msg_v01 cap;
	struct qmi_wlanfw_cap_req_msg_v01 cap_request = { 0 };
	struct qmi_wlanfw_m3_info_req_msg_v01 m3 = { 0 };
	struct qmi_wlanfw_m3_info_resp_msg_v01 m3_reply = { 0 };
	uint32_t board_id = ATH12K_BOARD_ID_DEFAULT;
	uint64_t size = 0;
	int disk = 0;
	const uint8_t *image;
	uint8_t *copy;
	char name[24];

	bzero(&cap, sizeof(cap));
	if (!ask("target capability", QMI_WLANFW_CAP_REQ_V01, QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_cap_req_msg_v01_ei, &cap_request, qmi_wlanfw_cap_resp_msg_v01_ei, &cap, false)) {
		return false;
	}
	if (cap.board_info_valid) {
		board_id = cap.board_info.board_id;
	}
	printf("pcie: step 9: chip %#x family %#x, board id %#x%s, SoC %#x; firmware %#x built %s, %s\n",
	    cap.chip_info.chip_id, cap.chip_info.chip_family, board_id, cap.board_info_valid ? "" : " (not said)",
	    cap.soc_info.soc_id, cap.fw_version_info.fw_version, cap.fw_version_info.fw_build_timestamp, cap.fw_build_id);
	if (!send_file("regulatory database", "regdb.bin", ATH12K_QMI_BDF_TYPE_REGDB, board_id)) {
		return false;
	}
	snprintf(name, sizeof(name), "bdwlan.e%02x", board_id & 0xff);
	if (!send_file("board data", name, ATH12K_QMI_BDF_TYPE_ELF, board_id)) {
		return false;
	}
	image = redwood_image_file("fw", "phy_ucode20.elf", &size, &disk);
	copy = image != NULL && size != 0 ? mhi_pcie_dma_alloc(size, &m3.addr) : NULL;
	if (copy == NULL) {
		printf("pcie: step 9: no fw/phy_ucode20.elf, or no memory for it (%llu bytes)\n", size);
		return false;
	}
	memcpy(copy, image, (size_t)size);
	m3.size = (uint32_t)size;
	printf("pcie: step 9: M3 image fw/phy_ucode20.elf, %llu bytes at %#llx\n", size, m3.addr);
	return ask("M3 information", QMI_WLANFW_M3_INFO_REQ_V01, QMI_WLANFW_M3_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	           qmi_wlanfw_m3_info_req_msg_v01_ei, &m3, qmi_wlanfw_m3_info_resp_msg_v01_ei, &m3_reply, false);
}

/* ath12k_qmi_firmware_start(), mode "mission", without the shadow registers */
static void
mission(void)
{
	static struct qmi_wlanfw_wlan_cfg_req_msg_v01 cfg;
	struct qmi_wlanfw_wlan_cfg_resp_msg_v01 cfg_reply = { 0 };
	struct qmi_wlanfw_wlan_ini_req_msg_v01 ini = { 0 };
	struct qmi_wlanfw_wlan_ini_resp_msg_v01 ini_reply = { 0 };
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode = { 0 };
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply = { 0 };

	ini.enable_fwlog_valid = 1;
	ini.enable_fwlog = 1;
	if (!ask("the firmware's logging", ATH12K_QMI_WLANFW_WLAN_INI_REQ_V01, QMI_WLANFW_WLAN_INI_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_ini_req_msg_v01_ei, &ini, qmi_wlanfw_wlan_ini_resp_msg_v01_ei, &ini_reply, false)) {
		return;
	}
	bzero(&cfg, sizeof(cfg));
	cfg.host_version_valid = 1;
	strlcpy(cfg.host_version, ATH12K_HOST_VERSION_STRING, sizeof(cfg.host_version));
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = WLFW_PIPE_COUNT;
	for (uint32_t i = 0; i < WLFW_PIPE_COUNT; i++) {
		cfg.tgt_cfg[i].pipe_num = WLFW_PIPES[i].pipenum;
		cfg.tgt_cfg[i].pipe_dir = WLFW_PIPES[i].pipedir;
		cfg.tgt_cfg[i].nentries = WLFW_PIPES[i].nentries;
		cfg.tgt_cfg[i].nbytes_max = WLFW_PIPES[i].nbytes_max;
		cfg.tgt_cfg[i].flags = WLFW_PIPES[i].flags;
	}
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = WLFW_SERVICE_COUNT;
	for (uint32_t i = 0; i < WLFW_SERVICE_COUNT; i++) {
		cfg.svc_cfg[i].service_id = WLFW_SERVICES[i].service_id;
		cfg.svc_cfg[i].pipe_dir = WLFW_SERVICES[i].pipedir;
		cfg.svc_cfg[i].pipe_num = WLFW_SERVICES[i].pipenum;
	}
	if (!ask("the configuration", QMI_WLANFW_WLAN_CFG_REQ_V01, QMI_WLANFW_WLAN_CFG_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_cfg_req_msg_v01_ei, &cfg, qmi_wlanfw_wlan_cfg_resp_msg_v01_ei, &cfg_reply, false)) {
		return;
	}
	mode.mode = ATH12K_FIRMWARE_MODE_NORMAL;
	mode.hw_debug_valid = 1;
	ask("the mode: mission", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false);
}

void
mhi_pcie_wlfw(void)
{
	struct qmi_wlanfw_phy_cap_req_msg_v01 phy = {};
	struct qmi_wlanfw_phy_cap_resp_msg_v01 phy_reply = { 0 };
	static struct qmi_wlanfw_ind_register_req_msg_v01 ind;
	struct qmi_wlanfw_ind_register_resp_msg_v01 ind_reply = { 0 };
	uint32_t want_mission = 0;
	int error;

	wlfw.began = now_ms();
	error = qmi_handle_init(&handle, ATH12K_QMI_RESP_LEN_MAX, &wlfw_ops, wlfw_handlers);
	if (error == 0) {
		error = qmi_add_lookup(&handle, ATH12K_QMI_WLFW_SERVICE_ID_V01, ATH12K_QMI_WLFW_SERVICE_VERS_V01,
		    WLFW_SERVICE_INSTANCE);
	}
	if (error != 0 || !wait_for(&wlfw.server, 5000)) {
		printf("pcie: step 9: no WLFW service to talk to (error %d)\n", error);
		return;
	}
	if (ask("PHY capability", QMI_WLANFW_PHY_CAP_REQ_V01, QMI_WLANFW_PHY_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_phy_cap_req_msg_v01_ei, &phy, qmi_wlanfw_phy_cap_resp_msg_v01_ei, &phy_reply, false)) {
		printf("pcie: step 9: %u radios%s, board id %#x%s, single chip MLO %u\n", phy_reply.num_phy,
		    phy_reply.num_phy_valid ? "" : " (not said)", phy_reply.board_id, phy_reply.board_id_valid ? "" : " (not said)",
		    phy_reply.single_chip_mlo_support);
	}
	ind.client_id_valid = 1;
	ind.client_id = QMI_WLANFW_CLIENT_ID;
	ind.fw_ready_enable_valid = ind.fw_ready_enable = 1;
	ind.request_mem_enable_valid = ind.request_mem_enable = 1;
	ind.fw_mem_ready_enable_valid = ind.fw_mem_ready_enable = 1;
	ind.cal_done_enable_valid = ind.cal_done_enable = 1;
	ind.fw_init_done_enable_valid = ind.fw_init_done_enable = 1;
	if (!ask("the indications wanted", QMI_WLANFW_IND_REGISTER_REQ_V01, QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &ind, qmi_wlanfw_ind_register_resp_msg_v01_ei, &ind_reply, false) ||
	    !host_capability()) {
		return;
	}
	if (!wait_for(&wlfw.mem_requested, 10000) || !respond_memory()) {
		printf("pcie: step 9: stopped at the firmware's memory (asked for: %s)\n", wlfw.mem_requested ? "yes" : "no");
		return;
	}
	if (!wait_for(&wlfw.mem_ready, 10000) || !load_board()) {
		printf("pcie: step 9: stopped at the board data (FW_MEM_READY: %s)\n", wlfw.mem_ready ? "yes" : "no");
		return;
	}
	if (!wait_for(&wlfw.ready, 10000)) {
		printf("pcie: step 9: no FW_READY within 10 s of the M3 information\n");
		return;
	}
	(void)PE_parse_boot_argn("pcie_mission", &want_mission, sizeof(want_mission));
	if (want_mission) {
		mission();
	}
	printf("pcie: step 9: the end, %llu ms after the lookup%s\n", now_ms() - wlfw.began,
	    want_mission ? "" : "; the mode is left unsent (pcie_mission=1 sends it)");
}

#endif

#!/usr/bin/env python3
"""Host test for the loader kernel-lifecycle protection.

The marked blocks in xnuloader/loader.c (LC, LC-WRITE, LC-ENTER, LC-WATCH, LC-RETURN,
LC-USB, LC-FB, LC-POLL, LC-EXC, LC-TAIL) are extracted verbatim and compiled into a stub harness (the C text lives in tools/tests/loader-lifecycle-harness.py), so the
checks run production control flow and count production side effects: psci calls, the
log-partition query, block writes, hc_take_back, watchdog pets, kernel-memory reads, USB
sends and the event loop, including the verified-notification rule of the LC-USB and
LC-POLL blocks (statuses, buffer and length matching, the deadline order, the rev6 guards:
a short transfer event stops every owned request, a stopped endpoint answers every later event
with nothing, and only the recorded buffer and length may take over a queued receive).
The review-2 rules run as full sequences in sc_usb_dyn: the owned record vetoes every resubmit
path, a timeout or disconnect marks the request undeliverable so its late terminal only
releases it, the link wait (endpoint 0xff) ends on Connected so the command loop submits, and
an overlong Cancelled isolates instead of releasing.
The wait is bounded in every state (three seconds held, running or faulted, ten in a plain
command loop). A stale IN request can ask for one abort; only the shared verified terminal
rule releases its buffer. sc_usb_recover checks OUT delivery while IN is pending or isolated,
unavailable/failed aborts, malformed cancellations, normal completions, stable buffer
identity and bounded repeated waits. Idle command receives remain deliverable after repeated
wait timeouts; abandoned data receives still discard late bytes. PSCI expectations are raw spec numbers written independently
(TF-A include/lib/psci/psci.h, PSCI_RET_*), asserted against the constants inside the
extracted block, so a wrong constant fails instead of passing on a shared value.
The USB event/status/link enums and the event union are injected verbatim from
xnuloader/efi.h and pinned there by _Static_assert against raw numbers written from the
ABL interface the header documents (review L10); counterexamples swap the link-state order
and strip raw[8] from the injected text, and the compile must fail.
Coverage note: IRQ save/restore assembly becomes counted host calls. Return tests include device-held and legacy return reasons,
their result fields, and failed reset attempts. In the exception handler the
one IRQ mask instruction becomes a counted host no-op; the handler itself runs, with the
reset and poll-loop escapes routed through setjmp.
Usage: python3 tools/test-loader-lifecycle.py"""

import importlib.util
import re
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "xnuloader", "loader.c")
EFIH = os.path.join(ROOT, "xnuloader", "efi.h")
BUILD = os.environ.get("TMPDIR", "/tmp")
BLOCKS = ("LC", "LC-WRITE", "LC-IRQ", "LC-ENTER", "LC-WATCH", "LC-TAIL", "LC-RETURN", "LC-RESULT", "LC-RESET", "LC-USB", "LC-FB",
          "LC-POLL", "LC-EXC")

def read(path):
    with open(path) as f:
        return f.read()

def read_tu(path):
    """xnuloader/loader.c as the compiler sees it: its own fragments inlined in include order.

    The marked blocks live in those fragments; reassembling the text keeps every extraction
    and every mutation anchor working on the same text the compiler gets, with the blocks
    where the source says they are."""
    src = read(path)
    base = os.path.dirname(path)
    return re.sub(r'^#include "(loader_[A-Za-z0-9_]+\.inc)"\n',
                  lambda m: read(os.path.join(base, m.group(1))), src, flags=re.M)

def extract(src, name):
    m = re.search(r"/\* %s-BEGIN.*?%s-END \*/" % (name, name), src, re.S)
    if not m:
        sys.exit("block %s-BEGIN not found in loader.c" % name)
    return m.group(0)

def macro_value(src, name):
    m = re.search(r"#define\s+%s\s+\(?([0-9xA-Fa-fu]+)\)?" % name, src)
    if not m:
        sys.exit("macro %s not found" % name)
    return m.group(1).rstrip("u")

def struct_copy(src, name):
    m = re.search(r"struct %s \{.*?\n\};" % name, src, re.S)
    if not m:
        sys.exit("struct %s not found" % name)
    return m.group(0)

_spec = importlib.util.spec_from_file_location(
    "lifecycle_harness", os.path.join(os.path.dirname(os.path.abspath(__file__)), "loader-lifecycle-harness.py"))
_harness = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_harness)
_spec2 = importlib.util.spec_from_file_location(
    "lifecycle_harness2", os.path.join(os.path.dirname(os.path.abspath(__file__)), "loader-lifecycle-harness2.py"))
_harness2 = importlib.util.module_from_spec(_spec2)
_spec2.loader.exec_module(_harness2)
_spec3 = importlib.util.spec_from_file_location(
    "lifecycle_harness3", os.path.join(os.path.dirname(os.path.abspath(__file__)), "loader-lifecycle-harness3.py"))
_harness3 = importlib.util.module_from_spec(_spec3)
_spec3.loader.exec_module(_harness3)
PRELUDE = _harness.PRELUDE
SCENARIOS = (_harness.SCENARIOS.replace("static void ret_reset(int64_t *states)", _harness2.AFF_SCENARIO + "\nstatic void ret_reset(int64_t *states)")
             .replace("static void sc_usb_notify(void)", _harness2.USB2_SCENARIO + "\nstatic void sc_usb_notify(void)")
             .replace("sc_psci_spec();", "sc_affinity(); sc_device_return(); sc_reset_matrix(); sc_psci_spec();")
             .replace("sc_usb_guard(); sc_fault();", "sc_usb_guard(); sc_usb_dyn(); sc_fault();")
             .replace("static void sc_repeat(void)", _harness3.C_GROUP + "\nstatic void sc_repeat(void)")
             .replace("sc_fault(); sc_repeat();", "sc_fault(); sc_repeat(); cg_run_all();"))


MUTATIONS = [
	("mut_nowatch_started", "watch_", "\t\t\t*status = OTHER_CPU_NOT_STARTED;\n\t\t\treturn 1;   /* held at once: the charger and the storage are not touched this round */", "\t\t\t*status = OTHER_CPU_NOT_STARTED;"),
	("mut_nowatch_hung", "watch_", "\t\t\t*status = OTHER_CPU_HUNG;\n\t\t\treturn 1;   /* held at once: no block, USB or charger service past the decision */", "\t\t\t*status = OTHER_CPU_HUNG;"),
	("mut_samecore", "same_", "\tkernel_resources_held = !cores_off || reason == LOADER_RETURN_DEVICE_HELD;", "\tif (ran_other_core) {\n\t\tkernel_resources_held = !cores_off || reason == LOADER_RETURN_DEVICE_HELD;\n\t}"),
	("mut_device_hold", "device_", "\tkernel_resources_held = !cores_off || reason == LOADER_RETURN_DEVICE_HELD;", "\tkernel_resources_held = !cores_off;"),
	("mut_held_stage", "device_stage_", '((fault || (hold && !live)) && lc_starts(cmd, "oem stage "))', '0'),
	("mut_whitelist", "onissue_", "\treturn status == PSCI_E_NOT_SUPPORTED || status == PSCI_E_INVALID_PARAMS ||\n\t    status == PSCI_E_DENIED || status == PSCI_E_INVALID_ADDRESS;", "\treturn status != 0;"),
	("mut_already_on_const", "psci_spec_", "#define PSCI_E_ALREADY_ON       (-4)", "#define PSCI_E_ALREADY_ON       (-6)"),
	("mut_write_gate", "wlog_", "\tbio = ((kernel_resources_held || firmware_fault) && !active_watch) ? NULL : find_log_partition();", "\tbio = (kernel_resources_held && !active_watch) ? NULL : find_log_partition();"),
	("mut_usb_bound", "usb_idle_bounded_exit", "\t\tuint64_t limit = (running_live != NULL || kernel_resources_held || firmware_fault)\n\t\t    ? 3 * freq : USB_IDLE_WAIT_SECONDS * freq;", "\t\tuint64_t limit = (running_live != NULL || kernel_resources_held || firmware_fault)\n\t\t    ? 3 * freq : ~(uint64_t)0;   /* mutation: the plain command loop waits for ever again */"),
	("mut_enter_early", "bridge_", "\tif (status == 0) {\n\t\t/* the exemption covers the formal watch this answer opens", "\tif (1) {   /* rev1 bug: open on every answer */\n\t\t/* the exemption covers the formal watch this answer opens"),
	("mut_exc_keep", "fault_entry_revokes", "\tactive_watch = 0;\n\tif (kernel_resources_held) {\n\t\theld_irq_mask();", "\tif (kernel_resources_held) {\n\t\theld_irq_mask();"),
	("m_exc_entry_mask", "fault_entry_masks", "\t\theld_irq_mask();\n\t\tfirmware_fault = 1;", "\t\tfirmware_fault = 1;"),
	("m_exc_free_path", "fault_free", "\tif (kernel_resources_held || firmware_fault) {\n\t\t/* the same rule as reboot", "\tif (1) {\n\t\t/* the same rule as reboot"),
	("m_watch_notstarted_mask", "watch_notstarted_masks_first", "\t\t\theld_irq_mask();   /* the hold starts at the decision: nothing on this path runs first */\n", ""),
	("m_watch_hung_mask", "watch_hung_masks_first", "\t\t\theld_irq_mask();   /* before this round's watchdog call and before any service */\n", ""),
	("m_enter_no_mask", "bridge_", "\t} else if (kernel_resources_held) {", "\t} else if (0) {"),
	("m_tail_no_mask", "tail_held_masks", "\tif (kernel_resources_held || firmware_fault) {\n\t\t/* interrupts stay off while", "\tif (0) {\n\t\t/* interrupts stay off while"),
	("m_watch_pet_swap", "watch_notstarted_no_services", "@WATCH_PET_SWAP@"),
	("m_timeout_mark", "usb_rx_", "\t\t\tif (usb_out_owned && usb_command_bytes < 0) {\n\t\t\t\tusb_out_invalid = 1;\n\t\t\t}\n\t\t\tif (usb_in_owned) {\n\t\t\t\tusb_in_invalid = 1;\n\t\t\t}\n", ""),
	("mut_poll_guard", "usb_poll_", "\tif (!usb_rx_queued && !usb_out_lost && !usb_out_owned && usb_connected", "\tif (!usb_rx_queued && usb_connected"),
	("m_prequeue_owned_guard", "usb_poll_", " && !usb_out_owned && usb_connected &&", " && usb_connected &&"),
	("mut_fb_pre", "fb_", "\tfor (; *code && n < sizeof(msg) - 1; code++) {\n\t\tmsg[n++] = (uint8_t)*code;\n\t}\n\tfor (; *text && n < sizeof(msg) - 1; text++) {\n\t\tmsg[n++] = (uint8_t)*text;\n\t}\n\tusb_tx((const uint8_t *)msg, n);", "\tfor (; *code; code++) {\n\t\tusb_tx_buf[n++] = (uint8_t)*code;\n\t}\n\tfor (; *text && n < 63; text++) {\n\t\tusb_tx_buf[n++] = (uint8_t)*text;\n\t}\n\tusb_tx(usb_tx_buf, n);"),
	("mut_ev_active", "usb_notify_", '\tif (data->transfer.status == UsbDeviceTransferStatusActive ||\n\t    data->transfer.status > UsbDeviceTransferStatusCompleteError) {\n\t\treturn 0;   /* the controller may still work on the buffer: no read, no release, no resend */\n\t}', '\tif (data->transfer.status == UsbDeviceTransferStatusActive ||\n\t    data->transfer.status > UsbDeviceTransferStatusCompleteError) {\n\t\tif (out) { usb_out_owned = 0; usb_rx_queued = 0; } else { usb_in_owned = 0; }   /* rev4: every notification ended the request */\n\t\treturn 0;\n\t}'),
	("mut_ev_lostguard", "usb_guard_", '\tif (out ? usb_out_lost : usb_in_lost) {\n\t\treturn 0;   /* isolated keeps its buffer and its flags: a late event changes nothing */\n\t}\n', ""),
	("mut_ev_shortstop", "usb_guard_", '\tif (usb_out_owned || usb_rx_queued) {\n\t\tusb_stop_endpoint(1, "event too short to attribute");\n\t}\n\tif (usb_in_owned) {\n\t\tusb_stop_endpoint(0, "event too short to attribute");\n\t}', "	(void)0;"),
	("mut_ev_record", "usb_guard_", '\tif (usb_rx_queued) {\n\t\t/* the pending request stays exactly as it was submitted: only a caller using the\n		 * recorded buffer and length may take it over */\n		if (buf != usb_out_buf || len != usb_out_len) {\n			return -1;      /* refused: the on-file request and its ownership are untouched */\n		}\n		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */\n	} else {\n		if (usb_out_owned) {\n			/* the record alone says the buffer is with the controller (a timeout or a\n			 * disconnect left it there): sending again would hand the same buffer twice */\n			return usb_wait(USB_EP_OUT, 1);   /* wait out the terminal that will release it */\n		}\n		if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {\n			return -1;\n		}\n		usb_out_owned = 1;      /* the submit record is written only for a send that succeeded */\n		usb_out_buf = buf;      /* the notification is only believed for this buffer and length */\n		usb_out_len = len;\n		usb_command_bytes = command_wait ? 0 : -1;\n		usb_out_invalid = 0;    /* a new request starts with a fresh delivery state */\n	}\n', '\tif (usb_rx_queued) {\n		usb_rx_queued = 0;   /* rev5: any caller took it over and rewrote the metadata */\n	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {\n		return -1;\n	}\n	usb_out_owned = 1;\n	usb_out_buf = buf;\n	usb_out_len = len;\n'),
	("m_owned_veto", "dyn_", "\t\tif (usb_out_owned) {\n\t\t\t/* the record alone says the buffer is with the controller (a timeout or a\n\t\t\t * disconnect left it there): sending again would hand the same buffer twice */\n\t\t\treturn usb_wait(USB_EP_OUT, 1);   /* wait out the terminal that will release it */\n\t\t}\n", ""),
	("m_disconnect_clears_records", "usb_disconnect", "\t\t\t\t\tif (usb_out_owned) {\n\t\t\t\t\t\tusb_out_invalid = 1;\n\t\t\t\t\t}\n\t\t\t\t\tif (usb_in_owned) {\n\t\t\t\t\t\tusb_in_invalid = 1;\n\t\t\t\t\t}\n", "\t\t\t\t\tusb_out_owned = 0; usb_rx_queued = 0;   /* mutation: the disconnect drops the records */\n\t\t\t\t\treturn -1;   /* a disconnect ends this wait; the records stay on file */"),
	("m_disconnect_mark", "dyn_", "\t\t\t\t\tif (usb_out_owned) {\n\t\t\t\t\t\tusb_out_invalid = 1;\n\t\t\t\t\t}\n\t\t\t\t\tif (usb_in_owned) {\n\t\t\t\t\t\tusb_in_invalid = 1;\n\t\t\t\t\t}\n", ""),
	("m_connected_clears_owned", "usb_relink", "\t\t\t\tint was_connected = usb_connected;\n\t\t\t\tusb_connected = (data.device_state == UsbDeviceStateConnected);\n\t\t\t\tif (data.device_state != UsbDeviceStateConnected) {", "\t\t\t\tint was_connected = usb_connected;\n\t\t\t\tusb_connected = (data.device_state == UsbDeviceStateConnected);\n\t\t\t\tusb_out_owned = 0;   /* mutation: a reconnect forgets the record */\n\t\t\t\tif (data.device_state != UsbDeviceStateConnected) {"),
	("m_connected_clears_lost", "usb_relink", "\t\t\t\tint was_connected = usb_connected;\n\t\t\t\tusb_connected = (data.device_state == UsbDeviceStateConnected);\n\t\t\t\tif (data.device_state != UsbDeviceStateConnected) {", "\t\t\t\tint was_connected = usb_connected;\n\t\t\t\tusb_connected = (data.device_state == UsbDeviceStateConnected);\n\t\t\t\tusb_out_lost = 0;   /* mutation: a reconnect ends the isolation */\n\t\t\t\tif (data.device_state != UsbDeviceStateConnected) {"),
	("m_link_wait_exit", "dyn_", "\t\t\t\tif (!was_connected && endpoint == 0xff) {\n\t\t\t\t\t/* a link wait exists to learn of the connection: the command loop must\n\t\t\t\t\t * return and submit, holding every record exactly as it stands */\n\t\t\t\t\treturn 0;\n\t\t\t\t}\n", ""),
	("m_poll_disconnect_mark", "dyn_", "\t\t\t\tif (usb_out_owned) {\n\t\t\t\t\tusb_out_invalid = 1;\n\t\t\t\t}\n\t\t\t\tif (usb_in_owned) {\n\t\t\t\t\tusb_in_invalid = 1;\n\t\t\t\t}\n", ""),
	("m_connected_poll_clears", "usb_poll_", '\t\treturn;   /* the cable moved: the queue flag and the ownership records stay as they are */', '\t\tusb_out_owned = 0;   /* mutation: the cable event clears the record */\n\t\treturn;'),
	("m_cancel_success", "usb_wait_cancel", '\t\t\t\t\tif (kept == 1 && deliver) {\n\t\t\t\t\t\treturn (int64_t)bytes;\n\t\t\t\t\t}', '\t\t\t\t\tif ((kept == 1 || kept == 2) && deliver) {\n\t\t\t\t\t\treturn (int64_t)bytes;   /* mutation: Cancelled answers like a completion */\n\t\t\t\t\t}'),
	("m_ignore_other", "usb_cancel_frees", '\t\tusb_notification(0, &data, &n);\n\t\treturn;\n\t}\n\tif (data.transfer.endpoint_index != USB_EP_OUT) {', '\t\treturn;   /* mutation: the IN terminal is dropped unread */\n\t}\n\tif (data.transfer.endpoint_index != USB_EP_OUT) {'),
	("m_cancel_bound", "dyn_", "\t\tif (data->transfer.bytes_completed > sub_len) {\n\t\t\tusb_stop_endpoint(out, \"Cancelled over a length that was not submitted\");\n\t\t\treturn -1;   /* overlong like an over-OK: structurally unsound, nothing releases */\n\t\t}\n", ""),
	("m_isolation_frees", "usb_guard_buf", '\t\t\tusb_out_lost = 1;\n\t\t}', '\t\t\tusb_out_lost = 1;\n\t\t\tusb_out_owned = 0;   /* mutation: the isolation forgets the buffer */\n\t\t}'),
	("m_recover_no_age", "recover_young_kept", "\tif (!usb_in_invalid && usb_in_since != 0 && age < USB_IN_STALE_SECONDS) {\n\t\treturn -1;      /* young: its transfer may still be running, nothing to recover */\n\t}\n", ""),
	("m_poll_data_purpose", "interleave_poll_records_command_purpose", "\t\tusb_command_bytes = 0;\n\t\tusb_out_invalid = 0;", "\t\tusb_command_bytes = -1;\n\t\tusb_out_invalid = 0;"),
	("m_early_command_drop", "interleave_out_completion_saved", "usb_command_bytes = (int64_t)bytes;", "usb_command_bytes = 0;"),
	("m_command_idle_drop", "idle_command_wait_preserves_delivery", "if (usb_out_owned && usb_command_bytes < 0)", "if (usb_out_owned)"),
	("m_data_late_deliver", "idle_data_timeout_stays_invalid", "if (usb_out_owned && usb_command_bytes < 0)", "if (0)"),
	("m_recover_no_abort", "recover_abort_requested_once", "\t(void)USB->abort_xfer(USB_EP_IN);", "\t/* mutation: no abort request */"),
	("m_recover_forgets_record", "recover_abort_requested_once", "\tusb_in_abort_requested = 1;", "\tusb_in_abort_requested = 1;\n\tusb_in_owned = 0;"),
	("m_recover_repeat_abort", "recover_repoll_no_repeat_abort", "usb_in_lost || usb_in_abort_requested", "usb_in_lost"),
	("mut_ev_deadline", "usb_notify_", "@DEADLINE_SWAP@"),
	("m_aff_negative_pending", "ret_bad", '\t\t\tif (state == 0 || state == 2) {   /* on, or being started: the race may still settle */', '\t\t\tif (state != 1) {   /* mutation: an error is polled like a settling race */'),
	("m_aff_reask_off", "ret_aff_settles", '\t\t\tif (off_seen[i]) {\n\t\t\t\tcontinue;   /* an off core is never asked again */\n\t\t\t}\n', ''),
	("m_aff_budget_per_core", "ret_aff_shared", '\tuint64_t budget = freq / 10;   /* 100 ms of counter ticks, shared: never a per-core timer */', '\tuint64_t budget = freq / 10 * (uint64_t)cpu_count;   /* mutation: a budget per core */'),
	("m_aff_no_issue_gate", "ret_aff_shared", '\t\t\tif (READ_SYSREG(CNTVCT_EL0) - began > budget) {\n\t\t\t\twaiting = 1;   /* the budget stands on no further query */\n\t\t\t\tcontinue;\n\t\t\t}\n', ''),
	("m_aff_nofreq_skip", "ret_nofreq", '\tif (!polling && cpu_count > first) {\n\t\tprint("the counter runs at no frequency: cannot tell whether the cores are off\\n");\n\t\tcores_off = 0;\n\t}\n', '\t(void)polling;\n'),
]

def build_c(src):
    blocks = []
    for name in BLOCKS:
        txt = extract(src, name)
        if name == "LC-ENTER":
            txt = "static void\nrun_enter(void)\n{\n" + txt + "\n}\n"
        elif name == "LC-IRQ":
            # the mask is one AArch64 instruction pair: the host counts it instead of running it
            txt = txt.replace('__asm__ volatile ("msr daifset, #0xf\\nmsr S3_0_C12_C12_7, %0\\nisb" : : "r" ((uint64_t)0));',
                              "harness_irq_mask();")
            if "harness_irq_mask();" not in txt:
                sys.exit("LC-IRQ block: the mask instruction moved, update the test")
        elif name == "LC-EXC":
            txt = txt.replace('__asm__ volatile ("msr daifset, #0xf");', "harness_daifset();")
            txt = txt.replace("harness_daifset();\n\tfor (;;) {\n\t}",
                "harness_daifset();\n\twhile (halt_armed) { halt_armed = 0; longjmp(jb_exit, 3); }")
        elif name in ("LC-TAIL", "LC-RETURN"):
            # the restore writes two AArch64 registers: the host counts it instead of running it
            txt = txt.replace(
                '__asm__ volatile ("msr S3_0_C12_C12_7, %0\\nisb" : : "r" (held_group1));\n'
                '\t\t__asm__ volatile ("msr daif, %0" : : "r" (held_daif));',
                "harness_irq_restore();")
            if "harness_irq_restore();" not in txt:
                sys.exit("%s block: the restore instructions moved, update the test" % name)
            if name == "LC-TAIL":
                txt = "static int64_t\nrun_tail(int64_t status)\n{\n" + txt + "\n}\n"
        elif name == "LC-RESULT":
            txt = "static void\nreturn_result(uint64_t reason, uint64_t checkpoint, uint64_t slide)\n{\n" + txt + "\n}\n"
        elif name == "LC-POLL":
            txt = ("static void\npoll_once(void)\n{\n\tuint32_t msg = UsbDeviceEventNoEvent;\n\tuint64_t size = 0, n;\n"
                   "\tusb_device_event_data data;\n\tchar cmd[128];\n\n\tif (USB == NULL) {\n\t\treturn;\n\t}\n"
                   + txt + "\n}\nstatic void usb_poll_command(void) { poll_once(); }\n")
        blocks.append(txt)
    prelude = PRELUDE.replace("enum { CTX_LOG_PA = 0, CTX_KSP = 10 };\nstatic uint64_t loader_ctx[16];",
        re.search(r"enum \{\n\tCTX_TTBR0,.*?\};\nuint64_t loader_ctx\[CTX_COUNT\];", src, re.S).group(0))
    exc = re.search(r"enum \{ EXC_CTX_.*?\};", read(EFIH), re.S).group(0)
    usb = re.search(r"enum \{ UsbDeviceEventNoEvent.*?\} usb_device_event_data;", read(EFIH), re.S).group(0)
    for token, value in (("@C_PRELUDE@", _harness3.C_PRELUDE), ("@START@", macro_value(src, "START_SECONDS")), ("@SILENCE@", macro_value(src, "SILENCE_SECONDS")),
                         ("@FAULT_SERVE@", macro_value(src, "FAULT_SERVE_SECONDS")),
                         ("@SELFTEST@", macro_value(src, "CHECKPOINT_SELFTEST")), ("@MAGIC2@", macro_value(src, "KERNEL_LIVE_MAGIC2")),
                         ("@LIVE@", struct_copy(src, "kernel_live")), ("@EXC@", exc), ("@USB@", usb),
                         ("@DESCSET@", re.search(r"typedef struct \{[^}]*\} usb_device_descriptor_set;",
                                                 read(EFIH), re.S).group(0)),
                         ("@PSCI@", _harness2.PSCI_STUB)):
        prelude = prelude.replace(token, value)
    return prelude + "\n" + "\n".join(blocks) + "\n" + SCENARIOS + "\n"

def deadline_swap(src):
    """budget-before-completion mutant: swap the in-loop event handling with the budget
    and pet block. Both slices live inside the same for(;;) scope, so the swap always
    compiles; a completion that arrives at the deadline is thrown away by the mutant."""
    a0 = src.index("\t\t/* a completion in hand is verified before the budget")
    mid = src.index("\t\t/* the wait is bounded in every state", a0)
    a1 = src.index("\t\tif (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {\n\t\t\tlast_pet = READ_SYSREG(CNTVCT_EL0);\n\t\t\twdt_pet();\n\t\t}\n", mid)
    a1 += len("\t\tif (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {\n\t\t\tlast_pet = READ_SYSREG(CNTVCT_EL0);\n\t\t\twdt_pet();\n\t\t}\n")
    ev, rest = src[a0:mid], src[mid:a1]
    return ev + rest, rest + ev

def watch_pet_swap(src):
    """the round's watchdog pet moved ahead of the two timeout decisions: a path that is
    about to hold would then pet once after deciding (and, on a real core, before masking)."""
    a = src.index("\tif (!*arrived) {\n\t\tflush_dcache((uint64_t)(uintptr_t)&other_cpu_stage")
    b = src.index("\t/* Feed this core's watchdog through the requested budget")
    c = src.index("\t/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side")
    decisions, pet = src[a:b], src[b:c]
    return decisions + pet, pet + decisions

def compile_run(c_text, name):
    cpath = os.path.join(BUILD, "lifecycle-%s.c" % name)
    xpath = os.path.join(BUILD, "lifecycle-%s" % name)
    with open(cpath, "w") as f:
        f.write(c_text)
    r = subprocess.run(["cc", "-std=gnu11", "-O0", "-o", xpath, cpath], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("compile of %s failed:\n%s" % (name, r.stderr))
    try:
        r = subprocess.run([xpath], capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        # a mutation that breaks a loop bound leaves no FAIL line at all; the hang itself
        # is the signal, and the suite must not stall on it
        return 124, "HUNG: %s ran past the scenario budget" % name
    return r.returncode, r.stdout

def compile_fails(c_text, name):
    cpath = os.path.join(BUILD, "lifecycle-%s.c" % name)
    with open(cpath, "w") as f:
        f.write(c_text)
    r = subprocess.run(["cc", "-fsyntax-only", "-std=gnu11", cpath], capture_output=True, text=True)
    return r.returncode, r.stderr

def usb_abi_counterexamples(src):
    base = build_c(src)
    cases = (
        ("state enum order swapped", "enum { UsbDeviceStateConnected, UsbDeviceStateDisconnected };",
         "enum { UsbDeviceStateDisconnected, UsbDeviceStateConnected };"),
        ("event union without raw[8]", "\tuint64_t raw[8];\n", ""),
    )
    for label, old, new in cases:
        if old not in base:
            sys.exit("counterexample source changed: %s" % label)
        rc, err = compile_fails(base.replace(old, new, 1), "usbabi")
        if rc == 0 or "static assertion" not in err:
            sys.exit("counterexample not caught (%s): the asserts do not pin the %s" % (label, label))
        print("counterexample check: %-28s caught by _Static_assert" % label)

def main():
    src = read_tu(LOADER)
    rc, out = compile_run(build_c(src), "prod")
    print(out.rstrip())
    if rc != 0:
        sys.exit("production build failed")
    for entry in MUTATIONS:
        name, tag = entry[0], entry[1]
        if entry[2] == "@DEADLINE_SWAP@":
            old, new = deadline_swap(src)
        elif entry[2] == "@WATCH_PET_SWAP@":
            old, new = watch_pet_swap(src)
        else:
            old, new = entry[2], entry[3]
        patched = src.replace(old, new, 1)
        if patched == src:
            sys.exit("mutation %s: source text changed, update the test" % name)
        rc, mout = compile_run(build_c(patched), name)
        lines = [l for l in mout.splitlines() if l.startswith("FAIL")]
        if rc == 124:
            print("mutation check: %-22s caught by hang (scenario budget expired)" % name)
            continue
        if rc == 0:
            sys.exit("mutation %s survived: the tests miss it" % name)
        if not any(l.startswith("FAIL " + tag) for l in lines):
            sys.exit("mutation %s failed unexpectedly: %s" % (name, mout))
        print("mutation check: %-22s caught by %d checks (%s...)" % (name, len(lines), lines[0]))
    usb_abi_counterexamples(src)

if __name__ == "__main__":
    main()

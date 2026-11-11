#!/usr/bin/env python3
"""Compile the Iris INIT builder, queues and response parser against binary HFI fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


HFI_REL = Path('xnu/files/bsd/dev/annibale/annibale_iris_hfi.c')
PRIVATE_REL = HFI_REL.with_name('annibale_iris_private.h')
PUBLIC_REL = Path('xnu/files/bsd/sys/annibale_iris.h')
Q_DWORDS = 0x32000

HARNESS = r'''
#define ANNIBALE_IRIS_HOST_TEST 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "annibale_iris_private.h"

_Static_assert(sizeof(struct hfi_header) == 32, "HFI header layout");
_Static_assert(sizeof(struct hfi_packet) == 32, "HFI packet layout");
_Static_assert(sizeof(struct hfi_queue_header) == 56, "HFI queue layout");

struct iris_controller g_iris;
static unsigned sleeps, publish_after;
static uint8_t *reply;
static size_t reply_size;
static uint32_t reply_index;

static struct hfi_queue_header *fixture_queue(unsigned index)
{
	return (void *)(g_iris.uc_region.kva + 0x120u + 56u * index);
}

static void publish_reply(void)
{
	assert(reply_size % 4u == 0 && reply_size < 0xc8000u);
	uint32_t *ring = (void *)(g_iris.uc_region.kva + 0xc81c8u);
	for (size_t i = 0; i < reply_size / 4u; i++) {
		memcpy(&ring[(reply_index + i) % 0x32000u], reply + 4u * i, 4u);
	}
	fixture_queue(1)->write_idx = (reply_index + reply_size / 4u) % 0x32000u;
}

void iris_sleep_us(uint32_t us)
{
	assert(us == 250u);
	sleeps++;
	if (publish_after != 0 && sleeps == publish_after) {
		publish_reply();
	}
}

void iris_cache_clean_inval(const void *address, size_t size)
{
	(void)address;
	(void)size;
}

__HFI_SOURCE__

static uint8_t *read_blob(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
	long length = ftell(file);
	assert(length >= 0 && length <= 65536 && fseek(file, 0, SEEK_SET) == 0);
	*size = (size_t)length;
	uint8_t *data = calloc(1, *size + 4u);
	assert(data != NULL && fread(data, 1, *size, file) == *size);
	assert(fclose(file) == 0);
	return data;
}

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "FAIL: %s (ready=%u sleeps=%u)\n", #expression, ready, sleeps); \
		return 2; \
	} \
} while (0)

int main(int argc, char **argv)
{
	assert(argc == 14);
	size_t expected_size;
	reply = read_blob(argv[1], &reply_size);
	uint8_t *expected = read_blob(argv[2], &expected_size);
	bool want_ready = strtoul(argv[3], NULL, 0) != 0;
	uint32_t packet_seed = (uint32_t)strtoul(argv[4], NULL, 0);
	uint32_t header_seed = (uint32_t)strtoul(argv[5], NULL, 0);
	uint32_t cmd_index = (uint32_t)strtoul(argv[6], NULL, 0);
	reply_index = (uint32_t)strtoul(argv[7], NULL, 0);
	publish_after = (unsigned)strtoul(argv[8], NULL, 0);
	unsigned want_sleeps = (unsigned)strtoul(argv[9], NULL, 0);
	uint32_t fsr = (uint32_t)strtoul(argv[10], NULL, 0);
	unsigned cmd_mode = (unsigned)strtoul(argv[11], NULL, 0);
	unsigned msg_mode = (unsigned)strtoul(argv[12], NULL, 0);
	unsigned want_dbg_drain = (unsigned)strtoul(argv[13], NULL, 0);

	g_iris.uc_region.kva = (uintptr_t)calloc(1, 0x300000u);
	g_iris.sfr_region.kva = (uintptr_t)calloc(1, 0x1000u);
	g_iris.mmio = (uintptr_t)calloc(1, 0xf0000u);
	g_iris.smmu = (uintptr_t)calloc(1, 0x100000u);
	assert(g_iris.uc_region.kva && g_iris.sfr_region.kva && g_iris.mmio && g_iris.smmu);
	g_iris.next_packet_id = packet_seed;
	g_iris.next_header_id = header_seed;
	g_iris.smmu_page = 4096u;
	g_iris.smmu_pages = 128u;
	g_iris.smmu_cb = 7u;
	*(uint32_t *)(g_iris.smmu + 135u * 4096u + 0x58u) = fsr;
	struct hfi_queue_header *cmd = fixture_queue(0), *msg = fixture_queue(1);
	cmd->read_idx = cmd->write_idx = cmd_index;
	msg->read_idx = msg->write_idx = reply_index;
	fixture_queue(2)->write_idx = 1u;
	memcpy((void *)(g_iris.sfr_region.kva + 4u), "fixture debug", 14u);
	if (publish_after == 0) {
		publish_reply();
	}
	if (cmd_mode == 1) cmd->read_idx = (cmd_index + 1u) % 0x32000u;
	if (cmd_mode == 2) cmd->read_idx = 0x32000u;
	if (cmd_mode == 3) cmd->write_idx = 0x32000u;
	if (msg_mode == 1) msg->read_idx = 0x32000u;
	if (msg_mode == 2) msg->write_idx = 0x32000u;
	uint32_t initial_cmd_read = cmd->read_idx, initial_cmd_write = cmd->write_idx;

	bool ready = iris_hfi_sys_init();
	CHECK(ready == want_ready);
	CHECK(sleeps == want_sleeps);
	CHECK(g_iris.next_packet_id == (uint32_t)(packet_seed + 8u));
	CHECK(g_iris.next_header_id == (uint32_t)(header_seed + 1u));
	CHECK(cmd->read_idx == initial_cmd_read);
	uint32_t doorbell = *(uint32_t *)(g_iris.mmio + 0xa0150u);
	if (cmd_mode == 0) {
		CHECK(doorbell == 1u);
		CHECK(cmd->write_idx == (cmd_index + expected_size / 4u) % 0x32000u);
		uint32_t *ring = (void *)(g_iris.uc_region.kva + 0x1c8u);
		for (size_t i = 0; i < expected_size / 4u; i++) {
			CHECK(memcmp(&ring[(cmd_index + i) % 0x32000u], expected + 4u * i, 4u) == 0);
		}
	} else {
		CHECK(doorbell == 0 && cmd->write_idx == initial_cmd_write);
	}
	if (want_dbg_drain) CHECK(fixture_queue(2)->read_idx == 1u);
	printf("PASS ready=%u sleeps=%u tx_bytes=%zu\n", ready, sleeps, expected_size);
	free((void *)g_iris.uc_region.kva);
	free((void *)g_iris.sfr_region.kva);
	free((void *)g_iris.mmio);
	free((void *)g_iris.smmu);
	free(expected);
	free(reply);
	return 0;
}
'''


def packet(packet_type=0x01000001, flags=1, packet_id=1, payload=b'', size=None):
    actual_size = 32 + len(payload) if size is None else size
    return struct.pack('<8I', actual_size, packet_type, flags, 0, 0, packet_id, 0, 0) + payload


def message(*packets, session=0, size=None, count=None):
    payload = b''.join(packets)
    actual_size = 32 + len(payload) if size is None else size
    actual_count = len(packets) if count is None else count
    return struct.pack('<8I', actual_size, session, 99, 0, 0, 0, 0, actual_count) + payload


def command_fixture(packet_seed, header_seed):
    words = [32 + 8 * 36, 0, (header_seed + 1) & 0xffffffff, 0, 0, 0, 0, 8]
    properties = [(0x01000001, 7, 1), (0x03000003, 0, 8), (0x03000004, 0, 32),
                  (0x03000005, 0, 16), (0x03000006, 0, 0), (0x03000007, 0, 1),
                  (0x03000008, 0, 1), (0x03000009, 0, 1)]
    for index, (kind, flags, value) in enumerate(properties, 1):
        words += [36, kind, flags, 1, 0, (packet_seed + index) & 0xffffffff, 0, 0, value]
    return struct.pack('<' + 'I' * len(words), *words)


def fixtures():
    result = []

    def add(name, data=b'', ready=False, sleeps=None, seed=0, header_seed=0,
            cmd_index=0, msg_index=0, delay=0, fsr=0, cmd_mode=0, msg_mode=0, drain=0):
        result.append(dict(name=name, data=data, ready=ready,
                           sleeps=(0 if ready else 1000) if sleeps is None else sleeps,
                           seed=seed, header_seed=header_seed, cmd_index=cmd_index,
                           msg_index=msg_index, delay=delay, fsr=fsr,
                           cmd_mode=cmd_mode, msg_mode=msg_mode, drain=drain))

    ack = packet()
    add('matching_success', message(ack), ready=True)
    add('success_and_information', message(packet(flags=3)), ready=True)
    for name, flags in [('flags_zero', 0), ('information_only', 2)]:
        add(name, message(packet(flags=flags)), drain=1)
    for name, flags in [('session_error_only', 4), ('success_and_session_error', 5)]:
        add(name, message(packet(flags=flags)), sleeps=0)
    for name, flags in [('system_error_flag', 8), ('success_and_system_error', 9)]:
        add(name, message(packet(flags=flags)), sleeps=0, drain=1)
    add('previous_packet_id', message(packet(packet_id=0)), drain=1)
    add('ubwc_last_packet_id', message(packet(packet_id=8)), drain=1)
    add('nonzero_seed', message(packet(packet_id=42)), ready=True, seed=41, header_seed=29)
    add('packet_id_wrap', message(packet(packet_id=0)), ready=True, seed=0xffffffff)
    add('non_core_session', message(ack, session=7), drain=1)
    prop = packet(packet_type=0x03000003, payload=struct.pack('<I', 8))
    add('property_only', message(prop), drain=1)
    add('open_command_only', message(packet(packet_type=0x01000003)), drain=1)
    add('property_then_init', message(prop, ack), ready=True)
    for name, kind in [('watchdog_error', 0x05000001), ('legacy_error', 0x05000008),
                       ('last_system_error', 0x05fffffe)]:
        add(name, message(packet(packet_type=kind, flags=0), ack), sleeps=0, drain=1)
    add('init_then_system_error', message(ack, packet(packet_type=0x05000001, flags=0)),
        sleeps=0, drain=1)
    add('init_then_error_flag', message(ack, packet(packet_type=0x03000003, flags=8)),
        sleeps=0, drain=1)
    add('init_then_session_error', message(ack, packet(packet_type=0x04000001, flags=0)), sleeps=0)
    add('system_error_begin_reserved', message(packet(packet_type=0x05000000, flags=0), ack), ready=True)
    add('system_error_end_reserved', message(packet(packet_type=0x05ffffff, flags=0), ack), ready=True)
    add('no_message_fsr_zero', drain=1)
    add('no_message_fsr_fault', fsr=0x1fe, drain=1)
    add('delayed_success', message(ack), ready=True, sleeps=7, delay=7)
    add('information_then_success', message(packet(flags=2)) + message(ack), ready=True, sleeps=1)
    add('previous_id_then_success', message(packet(packet_id=0)) + message(ack), ready=True, sleeps=1)
    add('property_message_then_success', message(prop) + message(ack), ready=True, sleeps=1)
    add('cmd_and_msg_ring_wrap', message(ack), ready=True,
        cmd_index=Q_DWORDS - 5, msg_index=Q_DWORDS - 5)
    add('header_only', message(), sleeps=0)
    add('short_header', struct.pack('<7I', 28, 0, 99, 0, 0, 0, 0), drain=1)
    for name, size in [('zero_packet_size', 0), ('short_packet_size', 31),
                       ('oversize_packet', 0xfffffffc)]:
        add(name, message(packet(size=size)), sleeps=0)
    add('short_packet_before_init', message(struct.pack('<I', 4) + packet(payload=b'\0' * 4), count=2),
        sleeps=0)
    add('unaligned_packets', message(packet(packet_type=0x03000003, flags=0, payload=b'x', size=33),
                                     packet(payload=b'xyz', size=35)), sleeps=0)
    add('unaligned_header_size', message(packet(payload=b'\0' * 4), size=65), sleeps=0)
    add('extra_packet_count', message(ack, count=2), sleeps=0)
    add('zero_packet_count', message(ack, count=0), sleeps=0)
    add('uncounted_trailer', message(ack + bytes(32), count=1), sleeps=0)
    add('init_then_bad_packet_size', message(ack, packet(packet_type=0x03000003, size=31)), sleeps=0)
    add('overlong_reply', message(packet(payload=bytes(1988))), drain=1)
    add('truncated_queue_message', message(ack)[:-4], drain=1)
    add('invalid_msg_read_index', message(ack), msg_mode=1, drain=1)
    add('invalid_msg_write_index', message(ack), msg_mode=2, drain=1)
    for name, mode in [('full_cmd_queue', 1), ('invalid_cmd_read_index', 2), ('invalid_cmd_write_index', 3)]:
        add(name, message(ack), cmd_mode=mode, sleeps=0)
    return result


def prefix(source):
    marker = 'static bool\niris_queue_buffer_ts('
    if source.count(marker) != 1:
        raise AssertionError('HFI source boundary changed')
    body = source[:source.index(marker)]
    barriers = {'__asm__ volatile("dsb ld" ::: "memory");': 1,
                '__asm__ volatile("dsb sy" ::: "memory");': 2}
    for instruction, count in barriers.items():
        if body.count(instruction) != count:
            raise AssertionError('HFI ARM barrier boundary changed')
        body = body.replace(instruction, '(void)0;')
    return body


def compile_variant(directory, source_root, source, private):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'annibale_iris_private.h').write_text(private)
    body = prefix(source)
    (directory / 'actual-hfi-prefix.txt').write_text(body)
    harness = directory / 'harness.c'
    harness.write_text(HARNESS.replace('__HFI_SOURCE__', body))
    binary = directory / 'harness'
    run = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                          '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
                          '-I' + str(source_root / 'xnu/files/bsd'), str(harness), '-o', str(binary)],
                         capture_output=True, text=True, timeout=60)
    (directory / 'compile.log').write_text(run.stdout + run.stderr)
    if run.returncode:
        raise AssertionError(f'{directory.name}: actual HFI prefix did not compile')
    return binary


def write_fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    cases = fixtures()
    manifest = {}
    for case in cases:
        path = directory / (case['name'] + '.hfi')
        path.write_bytes(case['data'])
        manifest[path.name] = dict(size=len(case['data']), sha256=hashlib.sha256(case['data']).hexdigest())
        case['reply_path'] = path
        command = command_fixture(case['seed'], case['header_seed'])
        tx_path = directory / f"command-{case['seed']}-{case['header_seed']}.hfi"
        tx_path.write_bytes(command)
        manifest[tx_path.name] = dict(size=len(command), sha256=hashlib.sha256(command).hexdigest())
        case['tx_path'] = tx_path
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return cases


def run_cases(binary, cases, directory):
    results = []
    for case in cases:
        args = [case['reply_path'], case['tx_path'], int(case['ready']), case['seed'],
                case['header_seed'], case['cmd_index'], case['msg_index'], case['delay'],
                case['sleeps'], case['fsr'], case['cmd_mode'], case['msg_mode'], case['drain']]
        run = subprocess.run([str(binary)] + [str(value) for value in args],
                             capture_output=True, text=True, timeout=5)
        (directory / (case['name'] + '.log')).write_text(run.stdout + run.stderr)
        results.append(dict(name=case['name'], passed=run.returncode == 0, returncode=run.returncode,
                            expected_ready=case['ready'], expected_sleeps=case['sleeps']))
    return results


def mutations():
    return {
        'capture_id_after_ubwc': ('source',
            '\tuint32_t init_packet_id = g_iris.next_packet_id;\n', '',
            '\tif (!iris_cmdq_write(&b)) {\n\t\treturn false;\n\t}\n\tiris_say("HFI INIT wait id=%u\\n", init_packet_id);',
            '\tuint32_t init_packet_id = g_iris.next_packet_id;\n\tif (!iris_cmdq_write(&b)) {\n\t\treturn false;\n\t}\n\tiris_say("HFI INIT wait id=%u\\n", init_packet_id);'),
        'drop_core_session': ('source', 'hdr->session_id == 0u && ', ''),
        'drop_init_type': ('source', 'hdr->session_id == 0u && pkt->type == HFI_CMD_INIT &&',
                           'hdr->session_id == 0u &&'),
        'drop_packet_id': ('source', 'pkt->packet_id == init_packet_id && ', ''),
        'drop_success_flag': ('source', '(pkt->flags & HFI_FW_FLAGS_SUCCESS) != 0', 'true'),
        'wrong_success_bit': ('private', '#define HFI_FW_FLAGS_SUCCESS            0x00000001u',
                              '#define HFI_FW_FLAGS_SUCCESS            0x00000002u'),
        'drop_system_error_flag': ('source', '(pkt->flags & HFI_FW_FLAGS_SYSTEM_ERROR) != 0', 'false'),
        'drop_system_error_range': ('source',
            '(pkt->type > HFI_SYSTEM_ERROR_BEGIN && pkt->type < HFI_SYSTEM_ERROR_END)', 'false'),
        'drop_session_error_flag': ('source', '(pkt->flags & HFI_FW_FLAGS_SESSION_ERROR) != 0', 'false'),
        'drop_session_error_range': ('source',
            '(pkt->type > HFI_SESSION_ERROR_BEGIN && pkt->type < HFI_SESSION_ERROR_END)', 'false'),
        'accept_before_remaining_packets': ('source', 'init_done = true;', 'return true;'),
        'drop_header_size': ('source', 'hdr->size != len || ', ''),
        'drop_packet_minimum': ('source', 'pkt->size < sizeof(*pkt)', 'false'),
        'drop_packet_alignment': ('source', ' || (pkt->size & 3u) != 0', ''),
        'drop_consumed_length': ('source', 'return off == len;', 'return true;'),
        'accept_timeout': ('source',
            'iris_say("HFI INIT timeout id=%u fsr=0x%08x\\n", init_packet_id, fsr);\n\treturn false;',
            'iris_say("HFI INIT timeout id=%u fsr=0x%08x\\n", init_packet_id, fsr);\n\treturn true;'),
        'drop_msg_wrap_copy': ('source',
            'memcpy(dst + first * 4u, &ring[0], (dwords - first) * 4u);',
            'memset(dst + first * 4u, 0, (dwords - first) * 4u);'),
        'drop_cmd_wrap_copy': ('source',
            'memcpy(&ring[0], src + first, (dwords - first) * 4u);',
            'memset(&ring[0], 0, (dwords - first) * 4u);'),
        'drop_msg_available_bound': ('source', 'dwords > avail', '(dwords > avail && false)'),
        'drop_msg_destination_bound': ('source', 'dwords * 4u > dst_cap', '(dwords * 4u > dst_cap && false)'),
        'drop_cmd_free_bound': ('source', 'dwords > free_dw', '(dwords > free_dw && false)'),
    }


def apply_mutation(source, private, mutation):
    target, *replacements = mutation
    boundary = source.index('static bool\niris_queue_buffer_ts(')
    text = source[:boundary] if target == 'source' else private
    for old, new in zip(replacements[::2], replacements[1::2]):
        if text.count(old) != 1:
            raise AssertionError('Mutation source boundary changed: ' + old)
        text = text.replace(old, new, 1)
    return (text + source[boundary:], private) if target == 'source' else (source, text)


def source_manifest(root):
    return {str(path): hashlib.sha256((root / path).read_bytes()).hexdigest()
            for path in (HFI_REL, PRIVATE_REL, PUBLIC_REL)}


def verify(args, directory):
    source = (args.source_root / HFI_REL).read_text()
    private = (args.source_root / PRIVATE_REL).read_text()
    cases = write_fixtures(directory / 'fixtures')
    results = {'source_manifest': source_manifest(args.source_root), 'cases': len(cases), 'variants': {}}
    candidate_dir = directory / 'candidate'
    binary = compile_variant(candidate_dir, args.source_root, source, private)
    candidate = run_cases(binary, cases, candidate_dir)
    results['variants']['candidate'] = candidate
    failures = [case['name'] for case in candidate if not case['passed']]
    if args.baseline_root:
        baseline_dir = directory / 'baseline'
        baseline_source = (args.baseline_root / HFI_REL).read_text()
        baseline_private = (args.baseline_root / PRIVATE_REL).read_text()
        binary = compile_variant(baseline_dir, args.baseline_root, baseline_source, baseline_private)
        baseline = run_cases(binary, cases, baseline_dir)
        results['baseline_source_manifest'] = source_manifest(args.baseline_root)
        results['variants']['baseline'] = baseline
        if all(case['passed'] for case in baseline):
            failures.append('unfixed_baseline_was_accepted')
    if args.mutations:
        for name, mutation in mutations().items():
            changed_source, changed_private = apply_mutation(source, private, mutation)
            mutation_dir = directory / name
            binary = compile_variant(mutation_dir, args.source_root, changed_source, changed_private)
            mutated = run_cases(binary, cases, mutation_dir)
            results['variants'][name] = mutated
            if all(case['passed'] for case in mutated):
                failures.append(name + '_was_accepted')
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if failures:
        raise AssertionError('HFI regression failed: ' + ', '.join(failures))
    print(f"PASS: {len(cases)} actual Iris INIT and queue cases")
    if args.baseline_root:
        rejected = [case['name'] for case in results['variants']['baseline'] if not case['passed']]
        print(f"PASS: actual unfixed baseline rejected in {len(rejected)} cases; first={rejected[0]}")
    if args.mutations:
        print(f"PASS: {len(mutations())} compiled protection variants rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--baseline-root', type=Path)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        verify(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='iris-hfi-init-') as name:
            verify(args, Path(name))


if __name__ == '__main__':
    main()

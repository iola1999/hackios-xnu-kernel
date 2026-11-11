#!/usr/bin/env python3
"""Compile actual Gen2 HFI/session functions against independent native-format fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

HFI = Path('xnu/files/bsd/dev/annibale/annibale_iris_hfi.c')
PRIVATE = HFI.with_name('annibale_iris_private.h')
SESSION = HFI.with_name('annibale_iris.c')
PUBLIC = Path('xnu/files/bsd/sys/annibale_iris.h')
FIXTURE = Path(__file__).with_name('iris-hfi-wire.fixture.c')
NATIVE_SIZES = Path(__file__).with_name('iris-hfi-native-sizes.json')


def packet(kind, flags, info, port, pid, payload=b''):
    payload += b'\0' * (-len(payload) % 4)
    return struct.pack('<8I', 32 + len(payload), kind, flags, info, port, pid, 0, 0) + payload


def header(sid, hid, packets):
    return struct.pack('<8I', 32 + sum(map(len, packets)), sid, hid, 0, 0, 0, 0,
                       len(packets)) + b''.join(packets)


def expected_open(row):
    hid, pid = 1000, 2000
    result = bytearray()

    def message(sid, fields):
        nonlocal hid, pid
        packets = []
        for kind, flags, info, port, payload in fields:
            pid += 1
            packets.append(packet(kind, flags, info, port, pid, payload))
        hid += 1
        result.extend(header(sid, hid, packets))

    u32 = lambda value: struct.pack('<I', value)
    message(0, [(0x01000003, 3, 1, 0, u32(101))])
    message(101, [(0x03000100, 0, 9, 0, u32(1 if row['codec'] == 1 else 3)),
                  (0x03000168, 0, 1, 1, u32(0)),
                  (0x03000103, 0, 1, 1, u32(640 << 16 | 480)),
                  (0x03000193, 0, 1, 1, u32(8))])
    addresses = []
    cursor = 0x30000000
    for size in row['internal']:
        addresses.append(cursor)
        cursor += size
    for index, kind, port in [(4, 12, 0), (0, 7, 1), (1, 10, 1), (2, 11, 1), (3, 8, 1)]:
        buffer = struct.pack('<IIQIIIIQI5I', kind, 0, addresses[index], 0,
                             row['internal'][index], 0, 0, 0, 0x200, 0, 0, 0, 0, 0)
        message(101, [(0x01000009, 1, 5, port, buffer)])
    change = [0x03000103, 0x03000105, 0x0300010f]
    if row['codec'] == 1:
        change += [0x03000120, 0x03000124, 0x03000128, 0x03000107, 0x03000108, 0x03000155]
    else:
        change += [0x03000124, 0x03000107, 0x03000108, 0x03000109, 0x03000155]
    # Native msm_vdec_set_min_output_count (0x2a168) sends U32 4 on input port 1
    # after PSC subscription and before ordinary properties and START.
    message(101, [(0x0100000b, 3, 11, 1, struct.pack('<' + 'I' * (len(change) + 1), 1, *change)),
                  (0x03000124, 0, 1, 1, u32(4)),
                  (0x0100000b, 3, 11, 1, struct.pack('<II', 2, 0x0300016a))])
    message(101, [(0x01000005, 3, 0, 1, b'')])
    return result


def expected_output(row):
    hid, pid = 1009, 2014
    result = bytearray()

    def message(fields):
        nonlocal hid, pid
        packets = []
        for kind, flags, info, port, payload in fields:
            pid += 1
            packets.append(packet(kind, flags, info, port, pid, payload))
        hid += 1
        result.extend(header(101, hid, packets))

    u32 = lambda value: struct.pack('<I', value)
    message([(0x03000184, 0, 1, 2, u32(1)), (0x03000101, 0, 9, 2, u32(1)),
             (0x03000104, 0, 13, 2, struct.pack('<II', 640 << 16 | 480, 640 << 16 | 240))])
    message([(0x01000008, 3, 1, 1, u32(0x0100000c))])
    properties = [(0x03000103, [640 << 16 | 480]), (0x03000105, [0, 0]),
                  (0x0300010f, [0x00080008])]
    if row['codec'] == 1:
        properties += [(0x03000120, [1])]
    properties += [(0x03000124, [2])]
    if row['codec'] == 1:
        properties += [(0x03000128, [0])]
    properties += [(0x03000107, [1]), (0x03000108, [40])]
    if row['codec'] == 2:
        properties += [(0x03000109, [0])]
    properties += [(0x03000155, [0])]
    change = [1, *[kind for kind, _ in properties]]
    fields = [(0x0100000b, 3, 11, 2, struct.pack('<' + 'I' * len(change), *change))]
    fields += [(kind, 0, 13 if kind == 0x03000105 else 1, 2,
                struct.pack('<' + 'I' * len(values), *values)) for kind, values in properties]
    output_properties = [2, 0x03000162]
    if row['codec'] == 1:
        output_properties += [0x03000121]
    fields += [(0x0100000b, 3, 11, 2, struct.pack('<' + 'I' * len(output_properties), *output_properties))]
    message(fields)
    message([(0x01000005, 3, 0, 2, b'')])
    dpb_base = 0x30000000 + sum(row['internal'][:5])
    for kind, index, address, size in (
            [(6, i, dpb_base + i * row['dpb_frame'], row['dpb_frame']) for i in range(2)] +
            [(2, i, 0x60000000 + i * row['raw'], row['raw']) for i in range(4)]):
        buffer = struct.pack('<IIQIIIIQI5I', kind, index, address, 0, size,
                             0, 0, 0, 0x200, 0, 0, 0, 0, 0)
        message([(0x01000009, 1, 5, 2, buffer)])
    return result


def psc(codec=1, variation='valid', native=False, malformed=None):
    fields = [(0x03000103, 10, [640 << 16 | 480]), (0x03000105, 13, [0, 0]),
              (0x0300010f, 1, [0x00080008])]
    if codec == 1:
        fields += [(0x03000120, 1, [1])]
    fields += [(0x03000124, 1, [2])]
    if codec == 1:
        fields += [(0x03000128, 1, [0])]
    fields += [(0x03000107, 9, [1]), (0x03000108, 9, [40])]
    if codec == 2:
        fields += [(0x03000109, 9, [0])]
    fields += [(0x03000155, 1, [0])]
    changes = {'depth': (0x0300010f, [0x000a000a]), 'zero-count': (0x03000124, [0]),
               'capacity': (0x03000124, [5]), 'min6': (0x03000124, [6]),
               'min7': (0x03000124, [7]), 'resolution': (0x03000103, [768 << 16 | 480]),
               'crop': (0x03000105, [1, 0]), 'interlace': (0x03000120, [0])}
    packets = []
    # Observed AVC metadata; payload values remain the supported host fixture values.
    reply_types = {0x03000105: 3, 0x0300010f: 10, 0x03000120: 10, 0x03000155: 10}
    for kind, info, values in fields:
        if native: info = reply_types.get(kind, info)
        if variation == 'missing' and kind == 0x03000155:
            continue
        if variation in changes and kind == changes[variation][0]:
            values = changes[variation][1]
        if variation == 'bad-payload' and kind == 0x03000103:
            info = 13
        payload = struct.pack('<' + 'I' * len(values), *values)
        flags, port = (0 if native else 1), 1
        if malformed and kind == malformed[0]:
            mode = malformed[1]
            if mode in ('short', 'short-buffer'): payload = payload[:-4]
            elif mode == 'wrong-info':
                info = 1 if kind == 0x03000105 else 3
                if info == 3: payload += b'\0' * 4
            elif mode == 'wrong-port': port = 2
            elif mode == 'information': flags = 2
        packets.append(packet(kind, flags, info, port, 3000 + len(packets), payload))
    packets.append(packet(0x0100000c, 0 if native else 1, 0, 1, 3099))
    if native:
        # cd20 AVC replies put SETTINGS_CHANGE first and use flags=0.
        order = [0x0100000c, 0x03000103, 0x03000105, 0x03000107, 0x03000108, 0x03000109,
                 0x0300010f, 0x03000120, 0x03000124, 0x03000128, 0x03000155]
        packets.sort(key=lambda p: order.index(struct.unpack_from('<I', p, 4)[0]))
    if malformed and malformed[1] == 'short-buffer':
        payload = struct.pack('<IIQIIIIQI5I', 1, 0, 0x26000000, 0, 0x400000,
                              0, 0, 0, 0, 0, 0, 0, 0, 0)
        packets.insert(0, packet(0x01000009, 0, 5, 1, 5000, payload))
    return header(101, 4000, packets)


def write_fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    sizes = json.loads(NATIVE_SIZES.read_text())
    rows = {row['codec']: row for row in sizes['cases'] if row['width'] == 640 and row['height'] == 480}
    cases = []
    for codec in (1, 2):
        path = directory / f'psc-{codec}.bin'
        path.write_bytes(psc(codec))
        wire = directory / f'open-{codec}.bin'
        wire.write_bytes(expected_open(rows[codec]))
        cases.append((f"wire-{'h264' if codec == 1 else 'hevc'}", path, wire))
        output = directory / f'psc-wire-{codec}.bin'
        output.write_bytes(expected_open(rows[codec]) + expected_output(rows[codec]))
        cases.append((f"wire-psc-{'h264' if codec == 1 else 'hevc'}", path, output))
        cases.append((f"psc-valid{'-hevc' if codec == 2 else ''}", path, wire))
        native_path = directory / f'psc-native-{codec}.bin'
        native_path.write_bytes(psc(codec, native=True))
        label = 'h264' if codec == 1 else 'hevc'
        cases.append(('psc-valid-native-' + label, native_path, wire))
        cases.append(('wire-psc-' + label + '-native', native_path, output))
    default = directory / 'psc-1.bin'
    expected = directory / 'open-1.bin'
    names = ['close-success', 'tlb-failure', 'input-immutable', 'output-copy', 'decode-psc',
             'alloc-clean', 'alloc-retained', 'open-caller-retained', 'close-caller-retained',
             'device-caller-retained', 'alloc-retained-later', 'stale-wire', 'cross-session',
             'wait-cross-session', 'truncated-later', 'later-session-buffer', 'later-system-buffer',
             'later-property-buffer', 'close-retry-stop', 'close-queue-failure',
             'payload-none', 'range-reject', 'drain-ack-only', 'drain-last-input-held',
             'drain-wrong-id-last', 'drain-complete', 'buffer-input-valid', 'buffer-raw-valid']
    for port in ('bs', 'raw'):
        names += [f'stop-{port}-{mode}' for mode in range(1, 8)]
    names += [f'close-error-{mode}' for mode in range(1, 8)]
    for kind in ('input', 'raw'):
        names += [f'buffer-{kind}-{field}' for field in ('wrong-sid', 'index', 'base', 'port', 'size', 'overflow')]
    names += ['buffer-raw-short', 'buffer-raw-readonly', 'buffer-raw-psc-last']
    cases.extend((name, default, expected) for name in names)
    for variation in ('missing', 'depth', 'zero-count', 'capacity', 'min6', 'min7',
                      'resolution', 'crop', 'interlace', 'bad-payload'):
        path = directory / f'psc-{variation}.bin'
        path.write_bytes(psc(variation=variation))
        cases.append(('psc-' + variation, path, expected))
    for kind in (0x03000105, 0x0300010f, 0x03000120, 0x03000155):
        for mode in ('short', 'wrong-info', 'wrong-port', 'information', 'short-buffer'):
            name = f'psc-metadata-{kind:x}-{mode}'
            path = directory / (name + '.bin')
            path.write_bytes(psc(native=True, malformed=(kind, mode)))
            cases.append((name, path, expected))
    for row in sizes['cases']:
        name = f"sizes-{'h264' if row['codec'] == 1 else 'hevc'}-{row['width']}x{row['height']}"
        path = directory / (name + '.bin')
        values = [row['width'], row['height'], row['codec'], int(row['supported']),
                  *row['internal'], row['dpb_frame'], row['raw']]
        path.write_bytes(struct.pack('<12I', *values))
        cases.append((name, default, path))
    return cases


def compile_variant(root, directory, sources=None, legacy=False):
    directory.mkdir(parents=True, exist_ok=True)
    sources = sources or {path: (root / path).read_text() for path in (HFI, PRIVATE, SESSION, PUBLIC)}
    (directory / 'annibale_iris_private.h').write_text(sources[PRIVATE])
    (directory / 'sys').mkdir(exist_ok=True)
    (directory / 'sys/annibale_iris.h').write_text(sources[PUBLIC])
    (directory / 'sys/ioccom.h').write_text(
        '#ifdef __APPLE__\n#include_next <sys/ioccom.h>\n'
        '#else\n#include <sys/ioctl.h>\n#endif\n')
    body = re.sub(r'__asm__ volatile\("dsb (?:ld|sy)" ::: "memory"\);', '(void)0;', sources[HFI])
    fixture = FIXTURE.read_text().replace('__HFI_SOURCE__', body).replace('__SESSION_SOURCE__', sources[SESSION])
    (directory / 'test.c').write_text(fixture)
    binary = directory / 'test'
    cmd = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-I', str(directory), str(directory / 'test.c'), '-o', str(binary)]
    if legacy:
        cmd.insert(1, '-DIRIS_LEGACY_WIRE=1')
    run = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    (directory / 'compile.log').write_text(run.stdout + run.stderr)
    if run.returncode:
        raise AssertionError(f'compile failed for {directory.name}: {run.stderr[:3000]}')
    return binary


def run_cases(binary, cases, directory):
    results = []
    environment = dict(os.environ)
    # Retained-session cases deliberately keep DMA allocations until process exit.
    environment['ASAN_OPTIONS'] = 'detect_leaks=0'
    for name, psc_path, expected in cases:
        run = subprocess.run([str(binary), name, str(psc_path), str(expected)],
                             capture_output=True, text=True, timeout=5, env=environment)
        (directory / (name + '.log')).write_text(run.stdout + run.stderr)
        results.append({'name': name, 'passed': run.returncode == 0, 'returncode': run.returncode})
    return results


def scoped(text, start, end, old, new):
    a = text.index(start)
    b = text.index(end, a) if end else len(text)
    section = text[a:b]
    if old not in section:
        raise AssertionError('Mutation boundary changed: ' + old)
    return text[:a] + section.replace(old, new, 1) + text[b:]


def mutations():
    initial_min = (
        '\t/* Native AVC/HEVC input subscription publishes the initial output minimum. */\n'
        '\tif (port == HFI_PORT_BITSTREAM &&\n'
        '\t    !pkt_add_u32(b, HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_HOST_FLAGS_NONE,\n'
        '\t    HFI_PAYLOAD_U32, port, 4u)) return false;\n')
    psc_subscription = (
        '\tif (!pkt_add(b, HFI_CMD_SUBSCRIBE_MODE, flags, HFI_PAYLOAD_U32_ARRAY,\n'
        '\t    port, payload, (count + 1u) * sizeof(uint32_t))) return false;\n')
    return {
        'psc_crop_u64': (HFI, 'static int\niris_read_property', 'static int\niris_start_output',
                         'pkt->payload_info == HFI_PAYLOAD_U64 ||', '', ['psc-valid-native-h264']),
        'psc_depth_packed': (HFI, 'static int\niris_read_property', 'static int\niris_start_output',
                             'index == PSC_DEPTH ||', '', ['psc-valid-native-h264']),
        'psc_coded_packed': (HFI, 'static int\niris_read_property', 'static int\niris_start_output',
                             'index == PSC_CODED ||', '', ['psc-valid-native-h264']),
        'psc_color_packed': (HFI, 'static int\niris_read_property', 'static int\niris_start_output',
                             ' || index == PSC_COLOR', '', ['psc-valid-native-h264']),
        'initial_min_missing': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                                initial_min, '', ['wire-h264', 'wire-hevc']),
        'initial_min_property': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                                 initial_min, initial_min.replace('HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT',
                                                                  'HFI_PROP_COMV_BUFFER_COUNT'),
                                 ['wire-h264', 'wire-hevc']),
        'initial_min_port': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                             initial_min, initial_min.replace('HFI_PAYLOAD_U32, port,',
                                                              'HFI_PAYLOAD_U32, HFI_PORT_RAW,'),
                             ['wire-h264', 'wire-hevc']),
        'initial_min_payload': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                                initial_min, initial_min.replace('HFI_PAYLOAD_U32,', 'HFI_PAYLOAD_U32_ENUM,'),
                                ['wire-h264', 'wire-hevc']),
        'initial_min_count': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                              initial_min, initial_min.replace('port, 4u', 'port, 8u'),
                              ['wire-h264', 'wire-hevc']),
        'initial_min_order': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                              psc_subscription + initial_min, initial_min + psc_subscription,
                              ['wire-h264', 'wire-hevc']),
        'open_core_address': (HFI, 'int\niris_hfi_session_open', 'static int\niris_copy_decoded',
                              'pkt_begin(&b, 0u);', 'pkt_begin(&b, s->wire_session_id);', ['wire-h264']),
        'hevc_codec': (PRIVATE, '#define HFI_CODEC_DECODE_HEVC', '#define HFI_COLOR_FMT_NV12',
                       '3u', '2u', ['wire-hevc']),
        'decoder_resolution_payload': (HFI, 'int\niris_hfi_session_open', 'static int\niris_copy_decoded',
                                       'HFI_PROP_BITSTREAM_RESOLUTION, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32,',
                                       'HFI_PROP_BITSTREAM_RESOLUTION, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_32_PACKED,',
                                       ['wire-h264', 'wire-hevc']),
        'nonsecure_buffer': (HFI, 'static bool\niris_queue_buffer_ts', 'static bool\niris_queue_buffer(',
                            'buf_flags | HFI_BUF_HOST_FLAGS_CB_NON_SECURE', 'buf_flags', ['wire-h264']),
        'old_bin_size': (HFI, 'static void\niris_internal_sizes', 'static int\niris_alloc_session_buffers',
                        '* 4u;', '* 2u;', ['wire-h264']),
        'buffer_input_index': (HFI, 'static int\niris_return_buffer', 'static int\niris_dispatch_message',
                               'hb->index != s->next_input_idx - 1u ||', '', ['buffer-input-index']),
        'buffer_input_base': (HFI, 'static int\niris_return_buffer', 'static int\niris_dispatch_message',
                              'hb->base_address != s->bitstream.iova ||', '', ['buffer-input-base']),
        'buffer_input_size': (HFI, 'static int\niris_return_buffer', 'static int\niris_dispatch_message',
                              'hb->buffer_size != s->bitstream.size', 'false', ['buffer-input-size']),
        'raw_readonly': (HFI, 'static int\niris_return_buffer', 'static int\niris_dispatch_message',
                         '(hb->flags & HFI_BUF_FW_FLAG_READONLY) != 0', 'false', ['buffer-raw-readonly']),
        'input_immutable': (HFI, 'int\niris_hfi_decode_frame', 'int\niris_hfi_session_flush',
                            's->input_queued || s->draining', 's->draining', ['input-immutable']),
        'reply_packet_id': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                           'pkt->packet_id == wait->packet_id && ', '', ['stop-bs-4']),
        'reply_port': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                      'pkt->port == wait->port', 'true', ['stop-bs-3']),
        'reply_success': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                         '(pkt->flags & HFI_FW_FLAGS_SUCCESS) != 0) wait->done', 'true) wait->done', ['stop-bs-5']),
        'reply_session': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                         'hdr->session_id == wait->session_id && ', '', ['wait-cross-session']),
        'reply_type': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                      'pkt->type == wait->type &&', 'true &&', ['stop-bs-2']),
        'system_error_priority': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                                  '(pkt->flags & HFI_FW_FLAGS_SYSTEM_ERROR) != 0 ||\n\t\t    '
                                  '(pkt->type > HFI_SYSTEM_ERROR_BEGIN && pkt->type < HFI_SYSTEM_ERROR_END)',
                                  'false', ['later-system-buffer', 'stop-bs-7']),
        'session_error_priority': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                                   '(pkt->flags & HFI_FW_FLAGS_SESSION_ERROR) != 0 ||\n\t\t    '
                                   '(pkt->type > HFI_SESSION_ERROR_BEGIN && pkt->type < HFI_SESSION_ERROR_END)',
                                   'false', ['later-session-buffer']),
        'property_error_priority': (HFI, '/* A malformed later property', 'static int\niris_receive',
                                    'if (s->error != 0) return s == target ? s->error : 0;', '',
                                    ['later-property-buffer']),
        'stop_both_ports': (HFI, 'int\niris_hfi_session_close', None,
                           'port <= HFI_PORT_RAW', 'port < HFI_PORT_RAW', ['close-success']),
        'stop_result': (HFI, 'int\niris_hfi_session_close', None,
                        'if (error != 0) return error;', '(void)error;', ['stop-bs-6']),
        'free_result': (HFI, 'int\niris_hfi_session_close', None,
                        '!iris_free_session_buffers(s)', 'iris_free_session_buffers(s) && false', ['tlb-failure']),
        'timeout_result': (HFI, 'static int\niris_wait_reply', 'int\niris_hfi_session_open',
                           'return ETIMEDOUT;', 'return 0;', ['close-error-6']),
        'validate_entire_message': (HFI, 'static int\niris_dispatch_message', 'static int\niris_receive',
                                    '!iris_validate_message(msg, len)', 'false', ['truncated-later']),
        'psc_complete': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                         '(s->psc_seen & required) != required ||', '', ['psc-missing']),
        'psc_resolution': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                           's->psc_values[PSC_RES] != ((s->width << 16) | s->height) ||', '', ['psc-resolution']),
        'psc_capacity': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                         'count > IRIS_RAW_POOL_COUNT ||', '', ['psc-capacity']),
        'psc_depth': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                      's->psc_values[PSC_DEPTH] != 0x00080008u ||', '', ['psc-depth']),
        'raw_nv12': (PRIVATE, '#define HFI_COLOR_FMT_NV12', '#define HFI_PORT_NONE',
                     '1u', '2u', ['wire-psc-h264', 'wire-psc-hevc']),
        'stride_packed': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                          'HFI_PAYLOAD_64_PACKED', 'HFI_PAYLOAD_U64', ['wire-psc-h264']),
        'psc_property_format': (HFI, 'static bool\niris_subscribe_port', 'static bool\niris_send_command',
                                'HFI_PAYLOAD_64_PACKED : HFI_PAYLOAD_U32;',
                                'HFI_PAYLOAD_64_PACKED : HFI_PAYLOAD_32_PACKED;', ['wire-psc-h264']),
        'dpb_single_frame': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                            's->dpb_frame_size, 0, 0', 's->intern_dpb.size, 0, 0', ['wire-psc-h264']),
        'psc_resume_reason': (HFI, 'static int\niris_start_output', 'struct iris_reply_wait',
                             'HFI_CMD_SETTINGS_CHANGE, NULL', 'HFI_CMD_DRAIN, NULL', ['wire-psc-h264']),
        'drain_ack_required': (HFI, 'static int\niris_finish_drain', 'static int\niris_collect_output',
                               '!s->drain_ack || ', '', ['drain-wrong-id-last']),
        'drain_last_required': (HFI, 'static int\niris_finish_drain', 'static int\niris_collect_output',
                                '!s->drain_last || ', '', ['drain-ack-only']),
        'drain_input_required': (HFI, 'static int\niris_finish_drain', 'static int\niris_collect_output',
                                 ' || s->input_queued', '', ['drain-last-input-held']),
        'iova_ranges': (HFI, 'static int\niris_alloc_session_buffers', 'enum {',
                        'total > IRIS_IOVA_SESSION_STRIDE ||\n\t    '
                        '(uint64_t)s->raw_frame_size * IRIS_RAW_POOL_COUNT > IRIS_IOVA_RAW_SESSION_STRIDE',
                        'false', ['sizes-h264-4096x4096']),
        'open_caller_retained': (SESSION, 'static int\niris_ioctl_open_session', 'static struct iris_session *',
                                'if (cleanup == 0) memset(slot, 0, sizeof(*slot));',
                                'if (true) memset(slot, 0, sizeof(*slot));', ['open-caller-retained', 'alloc-retained']),
        'close_caller_retained': (SESSION, 'case IRIS_IOC_CLOSE_SESSION: {', '\n\tdefault:',
                                 'if (err == 0) memset(s, 0, sizeof(*s));',
                                 'if (true) memset(s, 0, sizeof(*s));', ['close-caller-retained']),
        'device_caller_retained': (SESSION, 'static int\niris_dev_close', 'static int\niris_ioctl_get_info',
                                  'if (result == 0) memset(s, 0, sizeof(*s));',
                                  'if (true) memset(s, 0, sizeof(*s));', ['device-caller-retained']),
    }


def verify(args, directory):
    cases = write_fixtures(directory / 'fixtures')
    source = {path: (args.source_root / path).read_text() for path in (HFI, PRIVATE, SESSION, PUBLIC)}
    report = {'source_manifest': {str(p): hashlib.sha256(v.encode()).hexdigest() for p, v in source.items()},
              'native_commit': json.loads(NATIVE_SIZES.read_text())['commit'], 'variants': {}}
    binary = compile_variant(args.source_root, directory / 'candidate', source)
    result = run_cases(binary, cases, directory / 'candidate')
    report['variants']['candidate'] = result
    failures = [v['name'] for v in result if not v['passed']]
    if args.baseline_root:
        binary = compile_variant(args.baseline_root, directory / 'baseline', legacy=True)
        result = run_cases(binary, [c for c in cases if c[0].startswith('wire-')], directory / 'baseline')
        report['variants']['baseline'] = result
        if all(v['passed'] for v in result):
            failures.append('unfixed_baseline_accepted')
    if args.mutations and not failures:
        for name, (path, start, end, old, new, selected) in mutations().items():
            changed = dict(source)
            changed[path] = scoped(changed[path], start, end, old, new)
            binary = compile_variant(args.source_root, directory / name, changed)
            result = run_cases(binary, [c for c in cases if c[0] in selected], directory / name)
            report['variants'][name] = result
            if not result or all(v['passed'] for v in result):
                failures.append('mutation_accepted:' + name)
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if failures:
        raise AssertionError('HFI wire regression failed: ' + ', '.join(failures))
    print(f'PASS: {len(cases)} actual HFI/session cases')
    if args.baseline_root:
        print('PASS: actual unfixed baseline rejected by native OPEN/codec/buffer wire fixtures')
    if args.mutations:
        print(f'PASS: {len(mutations())} compiled protection variants rejected')


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
        with tempfile.TemporaryDirectory(prefix='iris-hfi-wire-') as name:
            verify(args, Path(name))


if __name__ == '__main__':
    main()

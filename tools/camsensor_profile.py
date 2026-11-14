"""Export checked annibale front mode 4 or back mode 24 as a RWCTAB03 profile.

The input stays a local vendor module. The exporter validates its V5 container,
pool references, settings and complete module identity before selecting the mode.
"""

import hashlib
import struct

from camsensor_v5 import Module, require


FRONT_SOURCE_SHA256 = '378bc647dea4676437c0c72e631f42b357d96ca8cf351c2829c7e80eb2801c72'
BACK_SOURCE_SHA256 = '68cd37a479ad2b8a681a1d965ca15005f5875c875b568664c9a9ec63e2033470'
PROFILE_FIELDS = (
    'bytes', 'sensor', 'mode', 'width', 'height', 'stride', 'bayer',
    'lane_assign', 'lane_count', 'settle_count', 'virtual_channel', 'data_type',
    'output_pixel_clock', 'line_length', 'frame_length', 'fps_milli',
    'init_count', 'mode_count', 'on_count', 'off_count', 'up_count', 'down_count',
)
HEADER = struct.Struct('<8s22I32s')
POWER = struct.Struct('<3I')
REGISTER = struct.Struct('<HHBBBBI')


def registers(settings):
    result = bytearray()
    for setting in settings:
        require(setting['slaveAddr'] is None and
                setting['serialized_optional_prefix'] == [None, None],
                'profile has unsupported register options')
        require(setting['regAddrType'] == 2 and setting['regDataType'] == 1 and
                setting['operation'] == 0 and len(setting['registerData']) == 1,
                'profile requires one 16-bit-address/8-bit-data write per setting')
        delay = setting['delayUs']
        require(delay in (None, 0, 4000), 'unverified profile register delay')
        result += REGISTER.pack(setting['registerAddr'], setting['registerData'][0],
                                2, 1, 0, 0, delay or 0)
    return result


def export(data, mode_index):
    module = Module(data)
    report = module.inspect()
    require(report['source_sha256'] in (FRONT_SOURCE_SHA256, BACK_SOURCE_SHA256),
            'unverified annibale sensor module')
    back = report['source_sha256'] == BACK_SOURCE_SHA256
    require(mode_index == (24 if back else 4), 'unverified native receive mode')
    require(len(report['resolutionInfo']) == 1 and len(report['initSettings']) == 1 and
            report['initSettings'][0]['sensorVersion'] == 0, 'unverified sensor version')
    mode = report['resolutionInfo'][0]['resolutionData'][mode_index]
    require(len(mode['streams']) == (2 if back else 1) and
            mode['streams'][0]['virtualChannel'] == [0] and
            mode['streams'][0]['dataType'] == 0x2b and mode['streams'][0]['bitWidth'] == 10 and
            mode['is3Phase'] == int(back), 'unverified VC0 RAW10 receive stream')
    if back:
        require(mode['streams'][1] == {'virtualChannel': [1], 'dataType': 0x2b,
                'xStart': 0, 'yStart': 0, 'width': 2560, 'height': 480,
                'bitWidth': 10, 'streamType': 2}, 'unverified back PDAF stream')
    stream = mode['streams'][0]

    ref, size = module.named['cameraModuleData']
    camera = data[module.pool + ref:module.pool + ref + size]
    configs = module.array(camera, 12, 246)
    require(len(configs) == 1, 'profile requires one module configuration')
    # ModuleConfiguration::Load writes laneAssign from +0x76 and comboMode from +0x7e.
    lane = module.optional(configs[0], 0x76, 2)
    combo = module.optional(configs[0], 0x7e, 1)
    require(lane is not None and combo == b'\0', 'unverified PHY lane/combo configuration')
    ref, size = module.named['sensorDriverData']
    driver = data[module.pool + ref:module.pool + ref + size]
    info = module.array(driver, 324, 16)[0]
    raw_mode = module.array(info, 8, 408)[mode_index]
    # ResolutionData::Load +0x5cdc04 stores this count in runtime mipiFlagsCount (+0x350).
    require(not module.array(raw_mode, 292, 4), 'unverified PHY MIPI flags')

    tables = (report['initSettings'][0]['initSetting'], mode['resSettings'],
              report['streamOnSettings'], report['streamOffSettings'])
    powers = (report['powerUpSequence'], report['powerDownSequence'])
    payload = bytearray()
    for sequence in powers:
        for entry in sequence:
            payload += POWER.pack(entry['configType'], entry['configValue'], entry['delayMs'])
    for table in tables:
        payload += registers(table)
    row_bytes = stream['width'] * 5 // 4
    require(stream['width'] % 4 == 0, 'partial RAW10 group')
    # Native line-based RDI with PLAIN_128 packs 16-byte WM units.
    stride = (row_bytes + 15) & ~15
    cfa = mode['colorFilterArrangement']
    bayer = {1: 3, 2: 2, 3: 1, 4: 0}[cfa] if back else {0: 3, 1: 2, 2: 1, 3: 0}[cfa]
    fields = (
        HEADER.size + len(payload), 1 if back else 2, mode_index, stream['width'], stream['height'], stride,
        bayer,
        struct.unpack('<H', lane)[0], mode['laneCount'], mode['settleTimeNs'], 0, stream['dataType'],
        mode['outputPixelClock'], mode['lineLengthPixelClock'], mode['frameLengthLines'],
        round(mode['frameRate'] * 1000), *(len(table) for table in tables),
        *(len(sequence) for sequence in powers),
    )
    blob = HEADER.pack(b'RWCTAB03', *fields, bytes.fromhex(report['source_sha256'])) + payload
    summary = dict(zip(PROFILE_FIELDS, fields))
    summary.update(source_sha256=report['source_sha256'], profile_sha256=hashlib.sha256(blob).hexdigest(),
                   row_bytes=row_bytes, frame_bytes=stride * stream['height'],
                   dma_bytes=(stride * stream['height'] + 255) & ~255,
                   source_stream=stream, ignored_streams=mode['streams'][1:], mipi_flags=[])
    return blob, summary

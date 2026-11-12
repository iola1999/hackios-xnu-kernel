"""Inspect V5.2.1 sensor modules using the saved SM8750 HAL's serialized layouts.

JSON retains all streams in each resolution and individual register operations.
The decoded subset covers identity, power, mode timing, initialization and stream
settings. It is an inspection result; the kernel does not consume this JSON.
"""

import hashlib
import math
import struct
import zlib


def require(condition, message):
    if not condition:
        raise ValueError(message)


class Module:
    def __init__(self, data):
        self.data = data
        require(len(data) >= 216, 'short V5 header')
        require(data.startswith(b'QTI Chromatix Header\0'), 'module signature')
        label = b'Parameter Parser V5.2.1 (2408261003)\0'
        require(data[0x28:0x28 + len(label)] == label, 'unsupported V5 parser version')
        require(self.words(data, 0x1c, 1)[0] == len(data) - 4, 'declared module length')
        require(self.words(data, len(data) - 4, 1)[0] == zlib.crc32(data[:-4]), 'module CRC32')
        require(struct.unpack_from('<HHI', data, 0x20) == (5, 0, 0), 'unsupported binary version')
        require(self.words(data, 0xa4, 1)[0] == 4, 'V5 section count')
        sections = {}
        end = 216
        for i in range(4):
            kind, offset, size = self.words(data, 0xa8 + i * 12, 3)
            require(kind not in sections and offset == end and size <= len(data) - 4 - offset,
                    'section bounds/order')
            sections[kind] = (offset, size)
            end = offset + size
        require(end == len(data) - 4 and set(sections) == {0, 1, 2, 3}, 'section coverage')
        self.pool, self.pool_size = sections[1]
        self.named = {}
        named_ids = set()
        offset, size = sections[0]
        require(size % 60 == 0, 'named symbol stride')
        for at in range(offset, offset + size, 60):
            name = data[at + 4:at + 36]
            require(b'\0' in name, 'symbol name terminator')
            name = name.split(b'\0', 1)[0].decode('ascii')
            fields = self.words(data, at + 36, 6)
            self.bounds(fields[3], fields[4])
            sid = self.words(data, at, 1)[0]
            require(name not in self.named and sid not in named_ids, 'duplicate named symbol')
            self.named[name] = fields[3:5]
            named_ids.add(sid)
        self.numeric = {}
        offset, size = sections[3]
        require(size % 16 == 0, 'numeric symbol stride')
        for at in range(offset, offset + size, 16):
            sid, ref, length, canonical = self.words(data, at, 4)
            require(sid not in self.numeric, 'duplicate numeric symbol')
            self.bounds(ref, length)
            self.numeric[sid] = (ref, length, canonical)
        require(set(self.numeric) == set(range(1, len(self.numeric) + 1)), 'numeric symbol numbering')
        for ref, length, canonical in self.numeric.values():
            require(canonical in self.numeric and self.numeric[canonical][:2] == (ref, length),
                    'numeric canonical alias')

    @staticmethod
    def words(raw, offset, count):
        require(0 <= offset <= len(raw) and count * 4 <= len(raw) - offset, 'field outside symbol')
        return struct.unpack_from('<' + 'I' * count, raw, offset)

    def bounds(self, ref, size):
        require(0 <= ref <= self.pool_size and 0 <= size <= self.pool_size - ref, 'pool reference bounds')

    def value(self, sid, expected=None):
        require(sid in self.numeric, 'missing numeric symbol')
        ref, size, _ = self.numeric[sid]
        require(expected is None or size == expected, 'referenced size')
        return self.data[self.pool + ref:self.pool + ref + size]

    def array(self, raw, offset, stride):
        count, sid = self.words(raw, offset, 2)
        values = self.value(sid, count * stride)
        return [values[i * stride:(i + 1) * stride] for i in range(count)]

    def optional(self, raw, offset, width=None):
        present, sid = self.words(raw, offset, 2)
        require(present in (0, 1), 'optional presence flag')
        payload = self.value(sid)
        if not present:
            return None
        require(width is None or len(payload) == width, 'optional payload size')
        return payload

    def optional_u32(self, raw, offset):
        payload = self.optional(raw, offset, 4)
        return None if payload is None else self.words(payload, 0, 1)[0]

    def settings(self, raw, offset):
        result = []
        for setting in self.array(raw, offset, 56):
            words = self.words(setting, 0, 14)
            count, sid = words[7:9]
            values = self.value(sid, count * 4)
            require(count > 0 and words[11] <= 4, 'register operation/data count')
            require(words[9] in (1, 2, 3, 4) and words[10] in (1, 2, 3, 4), 'register/data width')
            require(words[6] < 1 << (8 * words[9]), 'register exceeds address width')
            values = list(self.words(values, 0, count))
            # CreateRegisterSettingsCmd uses registerData[0] as the READ byte count.
            require(words[11] == 3 or all(v < 1 << (8 * words[10]) for v in values),
                    'register data exceeds data width')
            payload = self.optional(setting, 0, 2)
            slave = None if payload is None else struct.unpack('<H', payload)[0]
            extras = [self.optional(setting, at) for at in (8, 16)]
            result.append({
                'slaveAddr': slave, 'registerAddr': words[6],
                'registerData': values,
                'regAddrType': words[9], 'regDataType': words[10], 'operation': words[11],
                'delayUs': self.optional_u32(setting, 48),
                # The two additional optional fields are preserved without assigning schema names.
                'serialized_optional_prefix': [None if p is None else p.hex() for p in extras],
            })
        return result

    def mode(self, raw):
        words = self.words(raw, 0, 102)
        require(words[6] <= 10, 'unsupported CFA enum')
        streams = []
        for stream in self.array(raw, 28, 84):
            fields = self.words(stream, 0, 21)
            streams.append({
                'virtualChannel': [self.words(v, 0, 1)[0] for v in self.array(stream, 0, 4)],
                'dataType': fields[2],
                'xStart': fields[3], 'yStart': fields[4], 'width': fields[5], 'height': fields[6],
                'bitWidth': fields[7], 'streamType': fields[8],
            })
        require(streams and all(s['width'] and s['height'] for s in streams), 'empty mode stream')
        rate = struct.unpack_from('<d', raw, 72)[0]
        require(math.isfinite(rate) and rate > 0, 'invalid frame rate')
        require(1 <= words[24] <= 4 and words[32] in (0, 1), 'mode lane/phase value')
        return {
            'colorFilterArrangement': words[6], 'streams': streams,
            'lineLengthPixelClock': words[11], 'frameLengthLines': words[12],
            'minHorizontalBlanking': words[13], 'minVerticalBlanking': words[14],
            'outputPixelClock': words[15], 'horizontalBinning': words[16], 'verticalBinning': words[17],
            'frameRate': rate, 'laneCount': words[24], 'settleTimeNs': words[31], 'is3Phase': words[32],
            'resSettings': self.settings(raw, 140),
        }

    def inspect(self):
        require('sensorDriverData' in self.named, 'missing sensorDriverData')
        ref, size = self.named['sensorDriverData']
        require(size == 752, 'unsupported SensorDriverData layout')
        driver = self.data[self.pool + ref:self.pool + ref + size]
        length, sid = self.words(driver, 12, 2)
        name = self.value(sid, length)
        require(name.endswith(b'\0') and b'\0' not in name[:-1], 'sensor name')
        identity_fields = ('slaveAddress', 'regAddrType', 'regDataType', 'sensorIdRegAddr',
                           'sensorId', 'sensorIdMask', 'i2cFrequencyMode')
        identity = dict(zip(identity_fields, self.words(driver, 64, 7)))
        require(identity['regAddrType'] in (1, 2, 3, 4) and identity['regDataType'] in (1, 2, 3, 4),
                'identity register/data width')
        resolutions = []
        for info in self.array(driver, 324, 16):
            resolutions.append({
                'sensorVersion': self.optional_u32(info, 0),
                'resolutionData': [self.mode(raw) for raw in self.array(info, 8, 408)],
            })
        require(resolutions and all(r['resolutionData'] for r in resolutions), 'missing resolution data')
        init = []
        for raw in self.array(driver, 564, 20):
            extra = self.optional(raw, 4)
            init.append({'sensorVersion': self.words(raw, 0, 1)[0],
                         'serialized_optional_prefix': None if extra is None else extra.hex(),
                         'initSetting': self.settings(raw, 12)})
        powers = {}
        for label, offset in (('powerUpSequence', 108), ('powerDownSequence', 116)):
            powers[label] = [dict(zip(('configType', 'configValue', 'delayMs'), self.words(p, 0, 3)))
                             for p in self.array(driver, offset, 12)]
        return {
            'source_sha256': hashlib.sha256(self.data).hexdigest(), 'source_bytes': len(self.data),
            'binary_version': '5.0.0', 'parser_version': 'V5.2.1 (2408261003)',
            'sensorName': name[:-1].decode('ascii'), 'identity': identity, **powers,
            'resolutionInfo': resolutions, 'initSettings': init,
            'streamOnSettings': self.settings(driver, 492), 'streamOffSettings': self.settings(driver, 500),
            'groupHoldOnSettings': self.settings(driver, 508), 'groupHoldOffSettings': self.settings(driver, 516),
        }


def inspect(data):
    return Module(data).inspect()

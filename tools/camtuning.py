#!/usr/bin/env python3
"""Make a camera's colour tuning, as libannibale-media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning), out of the camera's tuning file of Android's camera stack
(/odm/lib64/camera/com.qti.tuned.<name>.bin of the phone: the vendor's, kept in fw-camera/ and never in
the repository; so is what this makes of it).

usage: camtuning.py TUNED.bin OUT.isp [MODULE.bin MODE]     (camtuning.py TUNED.bin -   lists what is there only)

The tuning file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file, laid out as the sensors'
module files are (see camsensor.py): a table of symbols { id, name[32], kind, ?, parent, offset, size }
and the data they point into. Two of its modules are taken, each as Chromatix keeps it -- a tree whose
first level is the flash (led_idx_data: 0 without one, the only one taken), the next the scene's light
(aec_data: from a lux index to another, Qualcomm's measure of how dark the scene is: higher is darker),
the last the light's colour temperature (cct_data: from a CCT to another, in kelvin):

  cc13 (the colour matrix, IPE's): per light and temperature a 3x3 matrix from the sensor's colours,
       white balanced, to linear sRGB, by rows R, G, B (each row sums to 1)
  lsc34 (lens shading): per light and temperature a gain for each of R, Gr, Gb and B at each point of
       a 17 x 13 grid laid over the sensor (corners on its corners, rows from the top)

and, of its white balance (Qualcomm's AWB), the reference lights (refPtV1): ten of them for every camera
of this phone, each as the sensor sees a grey under it, red over green and blue over green -- from the
bluest daylight to a candle, in the order 10000 K, D75, D65, D50 (whose gains are the AWB's first ones,
initDataV1), cool white and TL84 fluorescent tubes, U30, A (a filament lamp), horizon, candle. Each is
given the colour temperature that the tables above are picked by for it: its own, but 3800 K for U30, a
3000 K light without the infrared a filament gives -- as a white LED is -- whose tables are TL84's (the
wide camera under a warm white LED: its colour shading measured on a white sheet is TL84's table's to
within 3 %, the 2550-2900 K one's is 12 % off in red).

This phone (SM8750) has "Parameter Parser V5.2.1" files instead, read by v5_tables(): the colour matrix
of the IPE (cc14_ipe_v2), the lens shading of the TFE (lsc502_tfe_v2, the front end the back camera's
frames take on Android) and the same refPtV1. lsc502's leaves begin with the same four 17 x 13 grids as
lsc34's, R, Gr, Gb, B, the middle point 1.

The lens shading grids are laid over the whole of the sensor. A mode that reads part of it (the back
camera's mode 24: the middle 5120 x 3840 of 8192 x 6144) needs them laid over that part: given the
sensor's module file (com.qti.sensormodule.<name>.bin) and the mode, window() finds the part from the
mode's stream (where it starts and its size, in the mode's binned pixels) and mode 0's (the whole sensor,
unbinned), and on_window() takes each grid's gains there, between the file's points bilinear as the
processor takes them. Without them OUT.isp has the grids as the file has them.

OUT.isp: "RWCISP02"; the number of the matrices' light ranges and per range { lux index from, to (two
floats), the number of temperature ranges, per range { CCT from, to, the matrix (9 floats) } }; the
same for the lens shading, with per temperature range { CCT from, to, R, Gr, Gb, B grids (17 x 13 floats
each) }; the number of reference lights (0 where there are not the ten) and each { R/G, B/G, CCT }.
Everything little-endian, counts as 32-bit words.
"""
import struct
import sys

GRID = 17 * 13


def symbols(data):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, at = {}, table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += 0x38
    return syms


def module(syms, name, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree, without the flash"""
    led = next(v for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name)
    _, regions, sid = next(e for e in struct.iter_unpack('<3I', led) if e[0] == 0)
    out = []
    for i in range(regions):
        lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<4f2I', syms[sid][1], 24 * i)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            v = struct.unpack_from('<%df' % (2 + floats), cct, size * j)
            ranges.append((v[0], v[1], v[2:]))
        out.append((lux_from, lux_to, ranges))
    return out


# the colour temperature each of the ten reference lights picks the tables by (see above)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def references(syms):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    f = struct.unpack('<%df' % (len(v) // 4), v)
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def v5_tables(data):
    """(colour matrix tree, lens shading tree, reference lights) of a V5.2.1 tuning file, as module() and
    references() give them

    The file: a header, then sections by kind: 0 named symbols (60 bytes: id, name[32], six words, the
    5th and 4th the size and pool offset of the module's header), 1 the data pool, 3 numbered values (16
    bytes: id, pool offset, size, id of the same data). A module's header ends with the value of its tree,
    whose nodes are { from, to (floats), children, value of the children's nodes, 0 or 1, value of a
    leaf's data } (24 bytes). A module comes once for each use case; the first is the default one's.
    """
    words = lambda at, n: struct.unpack_from('<%dI' % n, data, at)
    sections = {kind: (at, size) for kind, at, size in (words(0xa8 + 12 * i, 3) for i in range(words(0xa4, 1)[0]))}
    pool = sections[1][0]
    at, size = sections[3]
    value = {sid: data[pool + ref:pool + ref + n] for sid, ref, n, _ in (words(a, 4) for a in range(at, at + size, 16))}
    at, size = sections[0]
    named = [(data[a + 4:a + 36].split(b'\0')[0].decode('latin1'), words(a + 36, 6)) for a in range(at, at + size, 60)]

    def first(name):
        f = next(f for n, f in named if n == name)
        return data[pool + f[3]:pool + f[3] + f[4]]

    def children(node):
        return [struct.unpack_from('<2f4I', value[node[3]], 24 * i) for i in range(node[2])]

    def depth(node):
        return 0 if node[2] == 0 else 1 + depth(children(node)[0])

    def tree(name, floats):
        header = first(name)
        node = struct.unpack('<2f4I', value[struct.unpack_from('<I', header, len(header) - 4)[0]])
        # The levels above the lux index and the colour temperature (DRC gain, exposure ratio, flash...) are
        # taken where they hold 1, or else 0 (the flash off): a preview's, assumed from their ranges only.
        while depth(node) > 2:
            nodes = children(node)
            node = next((n for n in nodes if n[0] <= 1 <= n[1]), next((n for n in nodes if n[0] <= 0 <= n[1]), nodes[0]))
        return [(lux[0], lux[1], [(c[0], c[1], struct.unpack_from('<%df' % floats, value[c[5]])) for c in children(lux)])
                for lux in children(node)]

    ccm = tree('cc14_ipe_v2', 9)
    lsc = tree('lsc502_tfe_v2', 4 * GRID)
    for _, _, ranges in lsc:
        for _, _, grids in ranges:
            if any(not 0.9 <= grids[c * GRID + GRID // 2] <= 1.1 for c in range(4)):
                sys.exit('camtuning.py: a lens shading grid of the file without 1 in its middle')
    for _, _, ranges in ccm:
        for _, _, m in ranges:
            if any(abs(sum(m[3 * r:3 * r + 3]) - 1) > 0.02 for r in range(3)):
                sys.exit('camtuning.py: a colour matrix of the file whose rows do not sum to 1')
    return ccm, lsc, references({0: ('refPtV1', first('refPtV1'))})


def window(module_data, mode_index):
    """(left, top, width, height) of the sensor mode's picture, each as a fraction of the whole sensor"""
    from camsensor_v5 import Module

    modes = Module(module_data).inspect()['resolutionInfo'][0]['resolutionData']
    whole, mode = modes[0], modes[mode_index]
    a, s = whole['streams'][0], mode['streams'][0]
    if whole['horizontalBinning'] != 1 or whole['verticalBinning'] != 1 or a['xStart'] or a['yStart']:
        sys.exit('camtuning.py: mode 0 of the module is not the whole sensor')
    h, v = mode['horizontalBinning'], mode['verticalBinning']
    part = (s['xStart'] * h / a['width'], s['yStart'] * v / a['height'], s['width'] * h / a['width'],
            s['height'] * v / a['height'])
    if part[0] + part[2] > 1 or part[1] + part[3] > 1:
        sys.exit('camtuning.py: the mode\'s picture is not within the sensor')
    return part


def on_window(grids, part):
    """the four 17 x 13 grids laid over part (window()'s) of the sensor instead of the whole of it"""
    left, top, width, height = part
    out = []
    for c in range(4):
        g = grids[c * GRID:(c + 1) * GRID]
        for j in range(13):
            y = (top + height * j / 12) * 12
            y0 = min(int(y), 11)
            fy = y - y0
            for i in range(17):
                x = (left + width * i / 16) * 16
                x0 = min(int(x), 15)
                fx = x - x0
                at = y0 * 17 + x0
                out.append((g[at] * (1 - fx) + g[at + 1] * fx) * (1 - fy) + (g[at + 17] * (1 - fx) + g[at + 18] * fx) * fy)
    return out


def main():
    if len(sys.argv) not in (3, 5):
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if data.startswith(b'QTI Chromatix Header') and b'Parameter Parser V5.2.1' in data[:0x60]:
        ccm, lsc, refs = v5_tables(data)
    elif data.startswith(b'QTI Chromatix Header') and b'Parameter Parser V3.0.1' in data[:0x60]:
        syms = symbols(data)
        ccm = module(syms, 'cc13', 9)
        lsc = module(syms, 'lsc34', 4 * GRID)
        refs = references(syms)
    else:
        sys.exit('camtuning.py: not a tuning file of the kind known here')
    if len(sys.argv) == 5:
        part = window(open(sys.argv[3], 'rb').read(), int(sys.argv[4]))
        print('the mode\'s picture: from %.4f, %.4f of the sensor, %.4f x %.4f of it' % part)
        lsc = [(lux_from, lux_to, [(cct_from, cct_to, on_window(v, part)) for cct_from, cct_to, v in ranges])
               for lux_from, lux_to, ranges in lsc]

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    shown = 'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(corners)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)


if __name__ == '__main__':
    main()

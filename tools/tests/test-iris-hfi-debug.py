#!/usr/bin/env python3
"""Exercise the production HFI diagnostics with native-format records and poisoned reuse."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HFI = Path('xnu/files/bsd/dev/annibale/annibale_iris_hfi.c')
FIXTURE = Path(__file__).with_name('iris-hfi-debug.fixture.c')


def packet(text=b'fatal fixture', size=None):
    # Native __flush_debug_queue: size at 0, four-word header, marker then text at 0x11.
    data = struct.pack('<4I', 0, 0x02000001, 0, 0) + b'\x01' + text + b'\0'
    data += b'\0' * (-len(data) % 4)
    return struct.pack('<I', len(data) if size is None else size) + data[4:]


def compile_harness(directory, source):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'annibale_iris_private.h').write_text((ROOT / HFI.with_name('annibale_iris_private.h')).read_text())
    body = re.sub(r'__asm__ volatile\("dsb (?:ld|sy)" ::: "memory"\);', '(void)0;', source)
    path = directory / 'test.c'
    path.write_text(FIXTURE.read_text().replace('__HFI_SOURCE__', body))
    command = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-I' + str(ROOT / 'xnu/files/bsd'), str(path), '-o', str(directory / 'test')]
    run = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (directory / 'compile.log').write_text(run.stdout + run.stderr)
    assert run.returncode == 0, run.stderr
    return directory / 'test'


def cases():
    good = packet()
    result = []

    def add(name, data=good, start=0, published=None, expected=None, mode='direct',
            text=b'fatal fixture', sfr=b'', sfr_size=4096):
        pub = len(data) if published is None else published
        end = (start + pub // 4) % 0x32000 if expected is None else expected
        result.append(dict(name=name, data=data, start=start, published=pub, expected=end,
                           mode=mode, text=text, sfr=sfr, sfr_size=sfr_size))

    add('complete')
    add('wrap', start=0x31ffe)
    add('max-record', packet(b'A' * 4074), text=b'A' * 4074)
    add('unterminated', packet(b'A' * 4074)[:-1] + b'A', text=b'A' * 4075)
    add('reuse-after-publish', mode='reuse')
    add('producer-append', mode='append')
    add('record-budget', good * 17, published=len(good) * 17,
        expected=len(good) // 4 * 16, text=b'fatal fixture' * 16)
    for size in [0, 4, 15, 19, 4096, 0xfffffffc, 0xffffffff]:
        add(f'bad-length-{size}', struct.pack('<I', size) + bytes(4092) if size == 4096 else packet(size=size), expected=0, text=b'')
    add('truncated', published=4, expected=0, text=b'')
    add('truncated-wrap', start=0x31ffe, published=4, expected=0x31ffe, text=b'')
    add('header-only', struct.pack('<4I', 16, 0x02000001, 0, 0), text=b'')
    add('bad-read', mode='bad-read', expected=0x32000, text=b'')
    add('bad-write', mode='bad-write', expected=0, text=b'')
    sfr = struct.pack('<I', 4096) + b'SFR independent\0'
    add('empty-with-sfr', data=b'', text=b'', sfr=sfr)
    add('invalid-queue-with-sfr', mode='bad-write', expected=0, text=b'', sfr=sfr)
    add('sfr-nonterminated', data=b'', text=b'', sfr=struct.pack('<I', 32) + b'S' * 28, sfr_size=32)
    for size in [0, 4, 33, 0xffffffff]:
        add(f'sfr-bad-size-{size}', data=b'', text=b'', sfr=struct.pack('<I', size) + b'S' * 28, sfr_size=32)
    add('sfr-short-allocation', data=b'', text=b'', sfr=b'AB', sfr_size=2)
    for mode in ['session', 'session-flag', 'unknown', 'init']:
        add(mode, mode=mode, sfr=sfr)
    return result


def run_cases(binary, directory, selected):
    results = []
    for c in selected:
        data, sfr = directory / (c['name'] + '.bin'), directory / (c['name'] + '.sfr')
        data.write_bytes(c['data']); sfr.write_bytes(c['sfr'])
        argv = [str(binary), c['mode'], str(data), str(c['start']), str(c['published']),
                str(c['expected']), str(sfr), str(c['sfr_size'])]
        run = subprocess.run(argv, capture_output=True, text=True, errors='replace', timeout=5,
                             env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        (directory / (c['name'] + '.log')).write_text(run.stdout + run.stderr)
        text = ''.join(re.findall(r'^iris: VPU debug \+\d+: (.*)$', run.stdout, re.M)).encode()
        passed = run.returncode == 0 and text == c['text']
        if c['sfr'].endswith(b'SFR independent\0'):
            passed &= 'VPU SFR +0: SFR independent' in run.stdout
        if c['name'] == 'sfr-nonterminated':
            passed &= 'VPU SFR +0: ' + 'S' * 28 in run.stdout and 'no terminator in 28 bytes' in run.stdout
        if c['name'].startswith('sfr-bad-size'):
            passed &= 'VPU SFR: invalid size=' in run.stdout and 'VPU SFR +0:' not in run.stdout
        results.append({'name': c['name'], 'exit': run.returncode, 'passed': bool(passed)})
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--baseline-ref', help='Optional Git revision whose diagnostics must fail the baseline cases')
    args = ap.parse_args(); out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / HFI).read_text(); selected = cases()
    binary = compile_harness(out / 'candidate', source)
    results = run_cases(binary, out / 'candidate', selected)
    assert all(x['passed'] for x in results), results
    mutations = {
        'length-bound': ('bytes >= sizeof(packet)', 'false', ['bad-length-4096']),
        'minimum-length': ('bytes < 16u', 'false', ['bad-length-4']),
        'word-alignment': ('(bytes & 3u) != 0', 'false', ['bad-length-19']),
        'available-bound': ('words > avail', 'false', ['truncated', 'truncated-wrap']),
        'wrap-copy': ('memcpy((uint8_t *)packet + first * 4u, ring, (words - first) * 4u);', '(void)0;', ['wrap']),
        'text-bound': ('strnlen(text, capacity)', 'strlen(text)', ['sfr-nonterminated']),
        'index-bound': ('rd >= IRIS_HFI_Q_DWORDS || wr >= IRIS_HFI_Q_DWORDS', 'false', ['bad-read']),
        'record-budget': ('records < 16u', 'records < 32u', ['record-budget']),
        'text-offset': ('(const char *)packet + 17u', '(const char *)packet + 16u', ['complete']),
        'sfr-bound': ('size <= 4u || size > capacity', 'size <= 4u', ['sfr-bad-size-33']),
        'sfr-empty-queue': ('iris_dump_sfr();', 'if (iris_qhdr(2u)->read_idx != iris_qhdr(2u)->write_idx) iris_dump_sfr();', ['empty-with-sfr']),
        'premature-publish': ('uint32_t first = IRIS_HFI_Q_DWORDS - rd;',
                              'qh->read_idx = (rd + words) % IRIS_HFI_Q_DWORDS; iris_cache_clean_inval(qh, sizeof(*qh));\n\t\tuint32_t first = IRIS_HFI_Q_DWORDS - rd;', ['reuse-after-publish']),
        'init-collection': ('iris_say("HFI INIT session error type=0x%08x flags=0x%08x\\n", pkt->type, pkt->flags);\n\t\t\t\t\tiris_drain_dbgq();',
                            'iris_say("HFI INIT session error type=0x%08x flags=0x%08x\\n", pkt->type, pkt->flags);', ['init']),
        'session-collection': ('iris_drain_dbgq();\n\t\t\tif (s != NULL)', '/* omitted */\n\t\t\tif (s != NULL)', ['session']),
    }
    negative = {}
    for name, (old, new, names) in mutations.items():
        assert old in source, name
        mutant = source.replace(old, new, 1)
        binary = compile_harness(out / name, mutant)
        negative[name] = run_cases(binary, out / name, [c for c in selected if c['name'] in names])
        assert any(not x['passed'] for x in negative[name]), name
    previous = []
    baseline_commit = None
    if args.baseline_ref:
        baseline_commit = subprocess.check_output(
            ['git', 'rev-parse', '--verify', '--end-of-options', args.baseline_ref + '^{commit}'],
            cwd=ROOT, text=True).strip()
        old_source = subprocess.check_output(['git', 'show', baseline_commit + ':' + str(HFI)], cwd=ROOT, text=True)
        binary = compile_harness(out / 'previous', old_source)
        previous = run_cases(binary, out / 'previous', [c for c in selected if c['name'] in ['complete', 'empty-with-sfr', 'session']])
        assert all(not x['passed'] for x in previous), previous
    report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'cases': results,
              'mutations': negative, 'previous': previous, 'baseline_commit': baseline_commit,
              'sanitizers': ['address', 'undefined']}
    (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    baseline_result = '; previous diagnostic failures confirmed' if previous else ''
    print(f'PASS {len(results)} cases; {len(negative)} rejected mutations{baseline_result}')


if __name__ == '__main__':
    main()

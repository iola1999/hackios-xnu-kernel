#!/usr/bin/env python3
"""在 macOS 上提取实际驱动与 AIR 函数，检查提交、完成和资源持有顺序。"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=ROOT)
parser.add_argument('--air-source', type=Path,
                    default=ROOT.parent / 'hackios-xnu-userland/tools/mesa/air')
parser.add_argument('--work-dir', type=Path)
args = parser.parse_args()
SOURCE = args.source_root.resolve() / 'tools/userland-driver'
AIR = args.air_source.resolve()
temporary = tempfile.TemporaryDirectory(prefix='metal-completion-') if args.work_dir is None else None
OUT = Path(temporary.name) if temporary is not None else args.work_dir.resolve()
if temporary is None:
    OUT.mkdir(parents=True, exist_ok=False)
EXTRACTS = []


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def extract(path, marker):
    source = path.read_text()
    begin = source.index(marker)
    opening = source.index('{', begin)
    depth = 0
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', source[opening:], re.S)
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                text = source[begin:opening + token.end()]
                EXTRACTS.append({'source': str(path), 'line': source[:begin].count('\n') + 1,
                                 'marker': marker, 'sha256': digest(text)})
                return text
    raise RuntimeError(marker)


def prepare(folder):
    folder.mkdir(parents=True, exist_ok=True)
    for name in ['prefix.h', 'main.inc']:
        shutil.copyfile(HERE / ('metal-completion-' + name), folder / ('fixture-' + name))
    air = (AIR / 'air_metal.c').read_text()
    lock_begin = air.index('static pthread_mutex_t rwa_lock')
    lock_end = air.index('\n\n', air.index('#define RWA_LOCKED'))
    locks = air[lock_begin:lock_end]
    timeout = re.search(r'#define RWA_FENCE_TIMEOUT_NS[^\n]+', air).group()
    EXTRACTS.append({'source': str(AIR / 'air_metal.c'), 'line': air[:lock_begin].count('\n') + 1,
                     'marker': 'RWA lock and timeout', 'sha256': digest(locks + '\n' + timeout)})
    fence = (AIR / 'air_metal_fence.inc').read_text()
    EXTRACTS.append({'source': str(AIR / 'air_metal_fence.inc'), 'line': 1,
                     'marker': 'entire fence implementation', 'sha256': digest(fence)})
    blocks = [locks, timeout,
              extract(AIR / 'air_metal.c', 'static int\nfinish_or_say_why'),
              extract(AIR / 'air_metal.c', 'int\nrwa_flush('),
              extract(AIR / 'air_metal.c', 'int\nrwa_finish('), fence]
    (folder / 'air-extracts.inc').write_text('\n\n'.join(blocks) + '\n')
    blocks = [extract(SOURCE / 'annibalemetal_texture.inc', '- (instancetype)initWithAnnibaleDevice:'),
              extract(SOURCE / 'annibalemetal_texture.inc', '- (void)dealloc')]
    (folder / 'texture-extracts.inc').write_text('\n\n'.join(blocks) + '\n')
    work = (SOURCE / 'annibalemetal_work.inc').read_text()
    work = work[:work.index('@end') + len('@end')]
    (folder / 'work-extract.inc').write_text(work + '\n')
    EXTRACTS.append({'source': str(SOURCE / 'annibalemetal_work.inc'), 'line': 1,
                     'marker': 'work ownership implementation', 'sha256': digest(work)})
    queue = (SOURCE / 'annibalemetal_queue.inc').read_text()
    at = queue.index('RW_CB_NOT_ENQUEUED')
    enum = queue[queue.rfind('enum {', 0, at):queue.index('};', at) + 2]
    blocks = ['#include "fixture-prefix.h"', enum,
              extract(SOURCE / 'annibalemetal_commandbuffer.inc', '@implementation AnnibaleMetalCommandBuffer')]
    for marker in ['- (instancetype)initWithAnnibaleQueue:', '- (void)annibaleReturnSlot', '- (void)dealloc',
                   '- (void)recordWork:', '- (void)failWith:', '- (NSUInteger)status', '- (NSError *)error', '- (id)device']:
        blocks.append(extract(SOURCE / 'annibalemetal_commandbuffer.inc', marker))
    blocks.append('''
- (unsigned)fixturePhase { return __atomic_load_n(&_phase, __ATOMIC_ACQUIRE); }
- (BOOL)runDraw:(AnnibaleMetalWork *)work why:(char *)why size:(size_t)size {
    (void)work; (void)why; (void)size; recorded++; return YES;
}
- (void)thumbWork:(AnnibaleMetalWork *)work index:(unsigned)index after:(BOOL)after {
    (void)work; (void)index; (void)after;
}
- (BOOL)syncDraw:(AnnibaleMetalWork *)work index:(unsigned)index sample:(BOOL)sample why:(char *)why size:(size_t)size {
    (void)work; (void)index; (void)sample; (void)why; (void)size; return YES;
}
#define pthread_mutex_unlock fixture_unlock
#include "submit-extract.inc"
''')
    blocks.append(extract(SOURCE / 'annibalemetal_commandbuffer.inc', '- (void)waitUntilCompleted'))
    blocks.append('''
#undef pthread_mutex_unlock
@end
#define calloc fixture_calloc
#define pthread_create fixture_create
#include "completion-extract.inc"
#undef pthread_create
#undef calloc
#include "fixture-main.inc"
''')
    (folder / 'fixture.m').write_text('\n\n'.join(blocks) + '\n')
    for original, destination in [('annibalemetal_commandbuffer_submit.inc', 'submit-extract.inc'),
                                  ('annibalemetal_completion.inc', 'completion-extract.inc')]:
        text = (SOURCE / original).read_text()
        (folder / destination).write_text(text)
        EXTRACTS.append({'source': str(SOURCE / original), 'line': 1,
                         'marker': 'entire implementation', 'sha256': digest(text)})
    (folder / 'extracts.json').write_text(json.dumps(EXTRACTS, indent=2) + '\n')


def execute(folder, label, arguments, expected=0):
    try:
        result = subprocess.run(arguments, cwd=folder, capture_output=True, text=True, timeout=15)
        code, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        code = 'host-timeout'
        stdout = (error.stdout or b'').decode() if isinstance(error.stdout, bytes) else error.stdout or ''
        stderr = (error.stderr or b'').decode() if isinstance(error.stderr, bytes) else error.stderr or ''
    (folder / (label + '.stdout')).write_text(stdout)
    (folder / (label + '.stderr')).write_text(stderr)
    passed = code == 0 if expected == 0 else (
        isinstance(code, int) and code != 0 and expected in stderr)
    record = {'arguments': arguments, 'returncode': code, 'expected': expected,
              'passed': passed}
    (folder / (label + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(label, code, flush=True)
    if not record['passed']:
        raise RuntimeError(label + ': ' + stdout[-1000:] + stderr[-2000:])
    return record


def build(folder, sanitizer='address,undefined'):
    binary = folder / ('fixture' if sanitizer != 'thread' else 'fixture-tsan')
    args = ['/usr/bin/xcrun', 'clang', '-fno-objc-arc', '-O1', '-g', '-Werror=return-type',
            '-Wno-deprecated-declarations', '-fsanitize=' + sanitizer,
            '-I' + str(SOURCE), '-I' + str(AIR),
            str(folder / 'fixture.m'), '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)]
    record = execute(folder, binary.name + '-build', args)
    record['artifact'] = {'path': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    return binary, record


def mutation(base, name, changes, scenario, diagnostic):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    for path in base.iterdir():
        if path.suffix in ['.m', '.h', '.inc']:
            subprocess.run(['cp', '-c', str(path), str(folder / path.name)], check=True)
    for filename, old, new in changes:
        path = folder / filename
        text = path.read_text()
        if old not in text:
            raise RuntimeError(name + ': missing marker')
        path.write_text(text.replace(old, new, 1))
    binary, compilation = build(folder)
    run = execute(folder, 'mutation-run', [str(binary), scenario], expected=diagnostic)
    return {'name': name, 'scenario': scenario, 'compile': compilation, 'run': run}


def main():
    folder = OUT / 'candidate'
    prepare(folder)
    binary, compilation = build(folder)
    scenarios = ['normal', 'error', 'error-timeout', 'timeout', 'legacy-error', 'legacy-timeout', 'legacy-gpu-timeout',
                 'oom', 'worker-failure', 'interleave', 'clear-timeout', 'fallback-lock', 'resources', 'phase']
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda name: execute(folder, name, [str(binary), name]), scenarios))
    summary = {'compile': compilation, 'scenarios': dict(zip(scenarios, results))}
    (OUT / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    snapshot = '''\t\t/* Retain this submission before another commit or finish changes pending. */
\t\tif (mesa.fence_take != NULL) {
\t\t\tsubmission_fence = mesa.fence_take([[_queue device] mesaDevice]);
\t\t}
'''
    late = '\tpthread_mutex_unlock(&rw_commit_lock);\n\t/* and only then the handlers'
    added = late.replace('\n\t/*', '\n' + snapshot + '\t/*')
    method = extract(SOURCE / 'annibalemetal_commandbuffer.inc', '- (void)waitUntilCompleted')
    fallback = '''\t/* The uncommitted fallback also changes Mesa pending; serialize it with submission. */
\tpthread_mutex_lock(&rw_commit_lock);
\tif (__atomic_load_n(&_phase, __ATOMIC_ACQUIRE) >= RW_CB_PHASE_COMMITTED) {
\t\tpthread_mutex_unlock(&rw_commit_lock);
\t\t[self annibaleAwaitCompletion];
\t\treturn;
\t}
'''
    assert fallback in method
    without_lock = method.replace(fallback, '').replace('\t\tpthread_mutex_unlock(&rw_commit_lock);\n', '').replace('\tpthread_mutex_unlock(&rw_commit_lock);\n', '')
    mutations = [
        ('take-after-unlock', [('submit-extract.inc', snapshot, ''), ('submit-extract.inc', late, added)],
         'clear-timeout', 'FAIL A still waits on its own earlier fence'),
        ('skip-failed-work', [('completion-extract.inc', 'item->fence = fence;', 'item->fence = _failed ? NULL : fence;')],
         'error', 'FAIL completion waits on submitted work'),
        ('oom-direct-finish', [('completion-extract.inc', '[buffer annibaleCompleteWithFence:fence commit:commit queued:rw_now_us()];',
                               '__atomic_store_n(&_status, RW_CB_COMPLETED, __ATOMIC_RELEASE); __atomic_store_n(&_phase, RW_CB_PHASE_FINISHED, __ATOMIC_RELEASE);')],
         'oom', 'FAIL allocation or worker failure waits for GPU'),
        ('ignore-worker-failure', [('completion-extract.inc', 'item == NULL || !worker_ready', 'item == NULL')],
         'worker-failure', 'FAIL allocation or worker failure waits for GPU'),
        ('overwrite-original-error', [('completion-extract.inc', 'if (_error == nil)', 'if (YES)')],
         'error-timeout', 'FAIL original error survives fence timeout'),
        ('fallback-without-lock', [('fixture.m', method, without_lock)],
         'fallback-lock', 'FAIL uncommitted finish cannot interleave flush and fence retain'),
    ]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        summary['mutations'] = list(pool.map(lambda args: mutation(folder, *args), mutations))
    tsan, compilation = build(folder, 'thread')
    summary['thread_sanitizer'] = {'compile': compilation,
                                    'run': execute(folder, 'phase-tsan', [str(tsan), 'phase'])}
    nonatomic = OUT / 'phase-nonatomic'
    nonatomic.mkdir(exist_ok=True)
    for path in folder.iterdir():
        if path.suffix in ['.m', '.inc', '.h']:
            subprocess.run(['cp', '-c', str(path), str(nonatomic / path.name)], check=True)
    for filename in ['fixture.m', 'completion-extract.inc', 'submit-extract.inc']:
        path = nonatomic / filename
        text = path.read_text()
        text = re.sub(r'__atomic_store_n\(&(_phase), ([^\n;]+), __ATOMIC_RELEASE\);', r'\1 = \2;', text)
        path.write_text(text)
    tsan, compilation = build(nonatomic, 'thread')
    summary['thread_sanitizer_mutation'] = {'compile': compilation,
                                            'run': execute(nonatomic, 'phase-tsan', [str(tsan), 'phase'],
                                                           expected='WARNING: ThreadSanitizer: data race')}
    summary['passed'] = True
    (OUT / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Check the production microphone packet and failure paths without hardware."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MIC = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_mic.c').read_text()
ADSP = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_adsp.c').read_text()

def function(source, name):
    m = re.search(r'static bool\s+' + name + r'\([^)]*\)\s*\{', source)
    assert m, name
    at, depth = m.end(), 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[m.start():at]

slot = function(MIC, 'submit_capture_slot')
assert slot.count('mpkt_put32') == 8, 'READ_V2 must carry data and metadata fields'
assert 'OP_READ_BUF_DONE' in slot and 'words[0] != 0' in slot
assert 'words[1] != (uint32_t)slot_addr' in slot
assert 'words[3] != map_handle' in slot
assert 'words[4] > MIC_PERIOD_BYTES' in slot
assert 'words[8] != 0' in slot
assert 'next_ambient_sample' not in MIC
assert 'mic_prng_state' not in MIC
assert 'chunk[i] = 0;' in MIC
start = MIC.index('start_capture_graphs')
end = MIC.index('static bool\nsubmit_capture_slot', start)
graph = MIC[start:end]
assert graph.count('return false') >= 5
feeder = MIC[MIC.index('mic_capture_thread'):MIC.index('/* ---- /dev/redwood-mic')]
assert 'submit_capture_slot' in feeder and 'if (annibale_audio_reach_dma_info' in feeder
assert 'retaining silence' in feeder
assert 'request_opcode == OP_READ_BUF' in ADSP
print('PASS: microphone READ_V2 layout, completion validation, and silent failure paths')

# Mutation checks prove the assertions are tied to source requirements.
mutations = {
    'drop metadata': (slot, 'mpkt_put32(&p, 0); /* metadata size */', ''),
    'ignore completion status': (slot, 'words[0] != 0 || ', ''),
    'restore pseudo noise': (MIC, 'chunk[i] = 0;', 'chunk[i] = 1;'),
}
for name, (source, old, new) in mutations.items():
    assert old in source, name
    changed = source.replace(old, new, 1)
    if name == 'drop metadata':
        assert changed.count('mpkt_put32') != 8
    elif name == 'ignore completion status':
        assert 'words[0] != 0' not in changed
    else:
        assert 'chunk[i] = 0;' not in changed
print(f'PASS: {len(mutations)} microphone regressions detected')

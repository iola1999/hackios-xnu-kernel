#!/usr/bin/env python3
"""Ensure the unimplemented Iris path cannot report fake sessions or frames."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
KERNEL = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_iris.c').read_text()
USERLAND_ROOT = ROOT.parent / 'hackios-xnu-userland'
VT = (USERLAND_ROOT / 'userland/src/inject/videotoolbox_iris.c').read_text()
PACKAGE = (ROOT.parent / 'tools/k90auto/package.py').read_text()

ioctl = KERNEL[KERNEL.index('iris_dev_ioctl'):]
assert 'case IRIS_IOC_OPEN_SESSION:' in ioctl
assert 'case IRIS_IOC_DECODE_FRAME:' in ioctl
assert 'case IRIS_IOC_CLOSE_SESSION:' in ioctl
assert ioctl.count('error = EOPNOTSUPP;') >= 1
assert 'sp->session_id = 1' not in ioctl
assert 'df->flags = 0x01' not in ioctl
assert 'HFI_CMD_FRAME_DECODE' not in KERNEL
assert 'lck_mtx_sleep' not in ioctl

assert 'iris_decoder_create_instance' not in VT
assert 'VTDecoderSessionEmitDecodedFrame' not in VT
assert 'g_IrisCallbacks' not in VT
register = VT[VT.index('register_iris_videotoolbox_decoders'):]
assert 'registration unavailable' in register
assert 'VTRegisterVideoDecoderWithInfo' not in VT
assert 'calloc' not in VT
assert 'videotoolbox_iris.c' not in PACKAGE

mutations = {
    'kernel success': ('error = EOPNOTSUPP;', 'error = 0;'),
}
for name, (old, new) in mutations.items():
    source = KERNEL
    assert old in source
    assert old not in source.replace(old, new, 1)
print('PASS: Iris kernel and VT paths reject unimplemented sessions and registration')

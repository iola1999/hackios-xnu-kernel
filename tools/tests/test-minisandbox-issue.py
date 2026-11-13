#!/usr/bin/env python3
"""Run the production file extension lifecycle and dispatch with host VFS/process stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / 'xnu/files/bsd/kern/kern_minimac.c').read_text()
    dispatch = source[source.index('static int\nminisandbox_policy_syscall('):source.index('/*\n * Privileges')]
    extension = (ROOT / 'xnu/files/bsd/kern/kern_minisandbox_extension.inc').read_text()
    fixture = Path(__file__).with_name('minisandbox-extension.fixture.c').read_text()
    marker = '/* MINI_SANDBOX_IMPLEMENTATION */'
    assert fixture.count(marker) == 1
    code = fixture.replace(marker, extension + '\n' + dispatch)
    with tempfile.TemporaryDirectory(prefix='minisandbox-extension-') as directory:
        cfile, binary = Path(directory) / 'test.c', Path(directory) / 'test'
        cfile.write_text(code)
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
            '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
            '-fsanitize=address,undefined', '-pthread', str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()

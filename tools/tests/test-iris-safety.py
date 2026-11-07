#!/usr/bin/env python3
"""Compile the Iris device implementation and verify unsupported operations."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
KERNEL = (ROOT / 'xnu/files/bsd/dev/annibale/annibale_iris.c').read_text()
VT = (ROOT.parent / 'hackios-xnu-userland/userland/src/inject/videotoolbox_iris.c').read_text()
PACKAGE = (ROOT.parent / 'tools/k90auto/package.py').read_text()

# Compile every production declaration/function with only host device-registry stubs.
# Hardware access has no stub and therefore cannot link into this test unnoticed.
HEADER = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define DEVFS_CHAR 1
#define UID_ROOT 0
#define GID_WHEEL 0
struct proc;
struct cdevsw {
 int (*d_open)(dev_t, int, int, struct proc *);
 int (*d_close)(dev_t, int, int, struct proc *);
 void (*d_read)(void), (*d_write)(void);
 int (*d_ioctl)(dev_t, u_long, caddr_t, int, struct proc *);
 void (*d_stop)(void), (*d_reset)(void), (*d_select)(void), (*d_mmap)(void), (*d_strategy)(void);
 int d_type;
};
static void no_operation(void) {}
#define eno_rdwrt no_operation
#define eno_stop no_operation
#define eno_reset no_operation
#define eno_select no_operation
#define eno_mmap no_operation
#define eno_strat no_operation
static int registry_calls, node_calls, hardware_accesses;
static int cdevsw_add(int major, const struct cdevsw *ops) {
 assert(major==-1 && ops->d_open && ops->d_ioctl); registry_calls++; return 42;
}
static void *devfs_make_node(dev_t dev, int type, int uid, int gid, int mode, const char *name) {
 (void)dev;
 assert(type==DEVFS_CHAR && uid==0 && gid==0 && mode==0666);
 assert(strcmp(name,"annibale-iris")==0); node_calls++; return (void *)1;
}
'''
MAIN = r'''
int main(void) {
 annibale_iris_init(); annibale_iris_init();
 assert(registry_calls==1 && node_calls==1 && hardware_accesses==0);
 const u_long commands[]={IRIS_IOC_OPEN_SESSION,IRIS_IOC_DECODE_FRAME,IRIS_IOC_CLOSE_SESSION,0};
 unsigned char data[128], original[128]; memset(data,0xa5,sizeof(data));
 memcpy(original,data,sizeof(data));
 for(int i=0;i<3;i++) {
  assert(iris_cdevsw.d_open(0,i,0,NULL)==EOPNOTSUPP);
  for(unsigned j=0;j<sizeof(commands)/sizeof(commands[0]);j++) {
   assert(iris_cdevsw.d_ioctl(0,commands[j],(caddr_t)data,0,NULL)==(j<3?EOPNOTSUPP:ENOTTY));
   assert(memcmp(data,original,sizeof(data))==0);
   assert(iris_cdevsw.d_ioctl(0,commands[j],NULL,0,NULL)==(j<3?EOPNOTSUPP:ENOTTY));
  }
  assert(iris_cdevsw.d_close(0,0,0,NULL)==0);
 }
 assert(registry_calls==1 && node_calls==1 && hardware_accesses==0);
 puts("PASS: Iris open/ioctl reject unsupported operations without hardware access or output changes");
}
'''


def run(source, directory, name):
    path = directory / (name + '.c')
    path.write_text(HEADER + re.sub(r'^#include[^\n]*\n', '', source, flags=re.M) + MAIN)
    binary = directory / name
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='iris-safety-') as name:
    directory = Path(name)
    result = run(KERNEL, directory, 'production')
    assert result.returncode == 0, result.stderr
    print("\n".join(line for line in result.stdout.splitlines() if line.startswith("PASS:")))
    mutations = {
        'open_success': ('return EOPNOTSUPP;', 'return 0;'),
        'ioctl_success': ('error = EOPNOTSUPP;', 'error = 0;'),
        'output_write': ('error = EOPNOTSUPP;', 'if (data) data[0] = 0; error = EOPNOTSUPP;'),
        'init_hardware': ('initialized = true;', 'initialized = true; hardware_accesses++;'),
        'open_hardware': ('return EOPNOTSUPP;', 'hardware_accesses++; return EOPNOTSUPP;'),
    }
    for label, (old, new) in mutations.items():
        assert old in KERNEL
        result = run(KERNEL.replace(old, new, 1), directory, label)
        assert result.returncode != 0, label
    print(f'PASS: {len(mutations)} compiled regressions are rejected')

assert 'iris_decoder_create_instance' not in VT
assert 'VTDecoderSessionEmitDecodedFrame' not in VT
assert 'g_IrisCallbacks' not in VT
register = VT[VT.index('register_iris_videotoolbox_decoders'):]
assert 'registration unavailable' in register
assert 'VTRegisterVideoDecoderWithInfo' not in VT
assert 'calloc' not in VT
assert 'videotoolbox_iris.c' not in PACKAGE
print('PASS: incomplete VideoToolbox registration remains outside the package')

#!/usr/bin/env python3
"""Compile MiniAMFI's signature hook on the host and check which ad hoc binaries become platform binaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/bsd/kern/kern_minimac.c"
PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define MAXPATHLEN 1024
#define Z_WAITOK 0
#define KERN_SUCCESS 0
#define KERN_NOT_FOUND 56
#define CS_ADHOC 0x2
#define CS_ENTITLEMENTS_VALIDATED 0x4000
#define CS_PLATFORM_BINARY 0x04000000
#define CS_SIGNED 0x20000000
#define kalloc_data(n, f) malloc(n)
#define kfree_data(p, n) free(p)
typedef int kern_return_t;
typedef int cpu_type_t;
struct label;
struct vnode { const char *path; };
struct cs_blob { struct cs_blob *csb_ro_addr; void *csb_entitlements; };
static bool platform_application, init_called;
static int vn_getpath(struct vnode *vp, char *buf, int *len)
{
    if (vp->path == NULL) return 2;
    snprintf(buf, (size_t)*len, "%s", vp->path);
    return 0;
}
static void miniamfi_blob_entitlements_init(struct cs_blob *blob) { init_called = true; blob->csb_entitlements = &platform_application; }
static void *csblob_os_entitlements_get(struct cs_blob *blob) { return blob->csb_entitlements; }
static kern_return_t miniamfi_osent_boolean(const void *ents, const char *name)
{
    assert(ents == &platform_application && strcmp(name, "platform-application") == 0);
    return platform_application ? KERN_SUCCESS : KERN_NOT_FOUND;
}
'''
TEST = r'''
static unsigned check(const char *path, unsigned in, bool entitled)
{
    struct vnode vp = { path };
    struct cs_blob blob = { NULL, NULL };
    unsigned flags = in;
    platform_application = entitled;
    init_called = false;
    assert(miniamfi_vnode_check_signature(&vp, NULL, 0, &blob, &flags, NULL, 0, 0, NULL, NULL) == 0);
    assert(init_called && (flags & (CS_SIGNED | CS_ENTITLEMENTS_VALIDATED)) == (CS_SIGNED | CS_ENTITLEMENTS_VALIDATED));
    return flags & CS_PLATFORM_BINARY;
}
int main(void)
{
    const char *app = "/private/var/containers/Bundle/Application/X/Weibo.app/Weibo";
    assert(check("/usr/libexec/lsd", CS_ADHOC, false));
    assert(check("/annibale/bin/annsh", CS_ADHOC, false));
    assert(check("/private/var/tmp/probe", CS_ADHOC, false));
    assert(check("/private/var/containers/BundleX/a", CS_ADHOC, false));
    assert(check(NULL, CS_ADHOC, false));
    assert(!check(app, CS_ADHOC, false));
    assert(check(app, CS_ADHOC, true));
    assert(!check(app, 0, true));
    assert(!check("/usr/libexec/lsd", 0, false));
    puts("PASS: installed applications are platform binaries only with platform-application");
    return 0;
}
'''


def main():
    source = SOURCE.read_text()
    begin = source.index("#define MINIAMFI_INSTALLED_BUNDLES")
    end = source.index("\n}\n", source.index("miniamfi_vnode_check_signature(struct vnode")) + 3
    with tempfile.TemporaryDirectory(prefix="miniamfi-platform-") as folder:
        folder = Path(folder)
        test = folder / "test.c"
        binary = folder / "test"
        test.write_text(PREAMBLE + source[begin:end] + TEST)
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

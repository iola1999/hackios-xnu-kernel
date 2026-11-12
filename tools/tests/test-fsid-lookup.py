#!/usr/bin/env python3
"""Execute mount ID allocation and XNU lookup functions on host mount fixtures.

The APFS mount identity statements are extracted from the driver. Mount-list
lookup and vfs_getbyid are unchanged XNU functions. Disk parsing, vnode lookup
and locking use fixtures; this does not validate on-device pathname traversal.
--allocation native substitutes vfs_getnewfsid in the generated host program.
--case stale-fsid-after-restart probes a stronger persistence requirement that
the native allocator does not promise; it is excluded from the default cases.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APFS = "xnu/files/bsd/miscfs/apfs/apfs_vfsops.c"


def function(source, name):
    start = source.index("\n" + name + "(")
    start = source.rfind("\n", 0, start) + 1
    return source[start:source.index("\n}\n", start) + 3]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#define fsid_t fixture_fsid_t
typedef struct { int32_t val[2]; } fsid_t;
typedef uint64_t ino64_t;
typedef void *vfs_context_t;
struct mount;
struct vnode { struct mount *mp; uint64_t ino; const char *path; unsigned refs; dev_t dev; };
typedef struct vnode *vnode_t;
struct vfsstatfs { fsid_t f_fsid; };
struct vtable { int vfc_typenum; };
struct mount {
    struct vfsstatfs mnt_vfsstat;
    struct vtable *mnt_vtable;
    int mnt_kern_flag, busy, refs, fail_busy, fail_ref;
    struct vnode root, file;
    TAILQ_ENTRY(mount) mnt_list;
};
typedef struct mount *mount_t;
TAILQ_HEAD(mount_head, mount);
static struct mount_head mountlist;
static unsigned short mntid_gen;
static int nblkdev = 32, list_locked;
#define MNTK_UNMOUNT 1
#define MNTK_PATH_FROM_ID 2
#define LK_NOWAIT 1
#undef makedev
#define makedev(x, y) ((dev_t)(((x) << 24) | (y)))
static void mount_list_lock(void) { assert(!list_locked); list_locked = 1; }
static void mount_list_unlock(void) { assert(list_locked); list_locked = 0; }
static int mount_iterref(mount_t mp, int locked) {
    assert(locked && list_locked);
    if (mp->fail_ref) return EBUSY;
    mp->refs++; return 0;
}
static void mount_iterdrop(mount_t mp) { assert(mp->refs == 1); mp->refs--; }
static int vfs_busy(mount_t mp, int flags) {
    assert(flags == LK_NOWAIT && !list_locked);
    if (mp->fail_busy) return EBUSY;
    mp->busy++; return 0;
}
static void vfs_unbusy(mount_t mp) { assert(mp->busy == 1); mp->busy--; }
static int VFS_ROOT(mount_t mp, vnode_t *vp, vfs_context_t ctx) {
    (void)ctx; assert(mp->busy == 1); *vp = &mp->root; (*vp)->refs++; return 0;
}
static int VFS_VGET(mount_t mp, ino64_t ino, vnode_t *vp, vfs_context_t ctx) {
    (void)ctx; assert(mp->busy == 1);
    if (ino != mp->file.ino) return ENOENT;
    *vp = &mp->file; (*vp)->refs++; return 0;
}
static void vnode_put(vnode_t vp) { assert(vp->refs == 1); vp->refs--; }
static int vfs_typenum(mount_t mp) { return mp->mnt_vtable->vfc_typenum; }
static dev_t vnode_specrdev(vnode_t vp) { return vp->dev; }
'''


CASES = r'''
static struct vtable apfs_type = {26}, ramfs_type = {18}, bindfs_type = {20};
static void reset(void) { TAILQ_INIT(&mountlist); mntid_gen = 0; list_locked = 0; }
static void add(mount_t mp, struct vtable *type, const char *root, const char *file,
                dev_t dev, uint32_t container, uint32_t volume) {
    memset(mp, 0, sizeof(*mp)); mp->mnt_vtable = type;
    mp->root = (struct vnode){.mp=mp, .ino=2, .path=root};
    mp->file = (struct vnode){.mp=mp, .ino=42, .path=file};
    mp->mnt_kern_flag = type == &bindfs_type ? 0 : MNTK_PATH_FROM_ID;
    if (type == &apfs_type) assign_apfs_identity(mp, dev, container, volume);
    else vfs_getnewfsid(mp);
    TAILQ_INSERT_TAIL(&mountlist, mp, mnt_list);
}
static int query(fsid_t id, uint64_t ino, const char *expected) {
    vnode_t vp = NULL;
    int error = vfs_getbyid(&id, ino, &vp, NULL);
    if (error) { printf("lookup error=%d\n", error); return error; }
    printf("lookup ino=%llu path=%s fsid={%d,%d}\n", (unsigned long long)ino,
           vp->path, vp->mp->mnt_vfsstat.f_fsid.val[0], vp->mp->mnt_vfsstat.f_fsid.val[1]);
    bool same = expected && !strcmp(vp->path, expected);
    vnode_put(vp);
    return same ? 0 : EXDEV;
}
static void released(void) {
    mount_t mp;
    TAILQ_FOREACH(mp, &mountlist, mnt_list)
        assert(!mp->refs && !mp->busy && !mp->root.refs && !mp->file.refs);
    assert(!list_locked);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2); const char *name = argv[1]; struct mount a, b, c;
    reset(); dev_t dev = makedev(3, 7);
    if (!strcmp(name, "multi-container-root") || !strcmp(name, "multi-container-file") ||
        !strcmp(name, "multi-volume-root") || !strcmp(name, "file-only-second") ||
        !strcmp(name, "legacy-devid")) {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        add(&b, &apfs_type, "/OS", "/OS/file", dev,
            !strcmp(name, "multi-volume-root") ? 0 : 1,
            !strcmp(name, "multi-volume-root") ? 1 : 0);
        fsid_t id = b.mnt_vfsstat.f_fsid;
        uint64_t ino = strstr(name, "file") ? 42 : 2;
        if (!strcmp(name, "file-only-second")) a.file.ino = 43;
        if (!strcmp(name, "legacy-devid")) id.val[1] = 0;
        const char *want = ino == 2 ? "/OS" : "/OS/file";
        assert(query(id, ino, want) == 0);
    } else if (!strcmp(name, "different-device")) {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        add(&b, &apfs_type, "/Other", "/Other/file", makedev(3, 8), 0, 0);
        assert(query(b.mnt_vfsstat.f_fsid, 2, "/Other") == 0);
    } else if (!strcmp(name, "native-filesystems")) {
        add(&a, &ramfs_type, "/Data", "/Data/file", 0, 0, 0);
        add(&b, &bindfs_type, "/Bind", "/Bind/file", 0, 0, 0);
        add(&c, &apfs_type, "/System", "/System/file", dev, 0, 0);
        assert(query(a.mnt_vfsstat.f_fsid, 42, "/Data/file") == 0);
        assert(query(b.mnt_vfsstat.f_fsid, 2, NULL) == EINVAL);
        assert(query(c.mnt_vfsstat.f_fsid, 2, "/System") == 0);
    } else if (!strcmp(name, "remount-current-id")) {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        add(&b, &apfs_type, "/OS", "/OS/file", dev, 1, 0);
        TAILQ_REMOVE(&mountlist, &b, mnt_list);
        add(&b, &apfs_type, "/OS", "/OS/file", dev, 1, 0);
        assert(query(b.mnt_vfsstat.f_fsid, 42, "/OS/file") == 0);
    } else if (!strcmp(name, "restart-same-order")) {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        add(&b, &apfs_type, "/OS", "/OS/file", dev, 1, 0);
        fsid_t old_a = a.mnt_vfsstat.f_fsid, old_b = b.mnt_vfsstat.f_fsid;
        reset();
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        add(&b, &apfs_type, "/OS", "/OS/file", dev, 1, 0);
        assert(!memcmp(&old_a, &a.mnt_vfsstat.f_fsid, sizeof(old_a)));
        assert(!memcmp(&old_b, &b.mnt_vfsstat.f_fsid, sizeof(old_b)));
    } else if (!strcmp(name, "stale-fsid-after-restart")) {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        fsid_t old = a.mnt_vfsstat.f_fsid;
        reset();
        add(&b, &apfs_type, "/OS", "/OS/file", dev, 1, 0);
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        printf("saved first=%d current first=%d\n", old.val[0], a.mnt_vfsstat.f_fsid.val[0]);
        /* This scenario deliberately requires the saved FSID to survive reordering. */
        assert(query(old, 2, "/System") == 0);
    } else {
        add(&a, &apfs_type, "/System", "/System/file", dev, 0, 0);
        fsid_t id = a.mnt_vfsstat.f_fsid;
        int expected = EINVAL;
        if (!strcmp(name, "unknown-mount")) id.val[0] ^= 0x40000000;
        else if (!strcmp(name, "unknown-inode")) expected = ENOENT;
        else if (!strcmp(name, "unmounting")) a.mnt_kern_flag |= MNTK_UNMOUNT;
        else if (!strcmp(name, "no-path-capability")) a.mnt_kern_flag &= ~MNTK_PATH_FROM_ID;
        else if (!strcmp(name, "busy")) a.fail_busy = 1;
        else if (!strcmp(name, "iterref-failure")) a.fail_ref = 1;
        else assert(!"unknown scenario");
        assert(query(id, !strcmp(name, "unknown-inode") ? 999 : 2, NULL) == expected);
    }
    released(); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xnu-root", type=Path, default=ROOT / "xnu-src/xnu-12377.121.6")
    parser.add_argument("--source-ref", help="Read the APFS mount function at this Git revision.")
    parser.add_argument("--allocation", choices=("source", "native"), default="source")
    parser.add_argument("--case", help="Run one scenario.")
    parser.add_argument("--keep-dir", type=Path, help="Keep generated source and output in an ignored directory.")
    args = parser.parse_args()
    apfs = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{args.source_ref}:{APFS}"], text=True) if args.source_ref else (ROOT / APFS).read_text()
    mount = function(apfs, "apfs_mount_common")
    identity = mount.split("sp->f_ffree = 0;", 1)[1].split('strlcpy(sp->f_fstypename, "apfs"', 1)[0]
    if args.allocation == "native":
        identity = "vfs_getnewfsid(mp);"
    source = PRELUDE
    subr = (args.xnu_root / "bsd/vfs/vfs_subr.c").read_text()
    for name in ("mount_list_lookupby_fsid", "vfs_getvfs_locked", "vfs_getnewfsid", "mount_lookupby_volfsid"):
        source += function(subr, name)
    source += function((args.xnu_root / "bsd/vfs/vfs_lookup.c").read_text(), "vfs_getbyid")
    source += "\nstatic void assign_apfs_identity(mount_t mp, dev_t dev, uint32_t container, uint32_t volume) {\n"
    source += "struct vfsstatfs *sp = &mp->mnt_vfsstat; struct { struct { uint32_t index; } am_vol; } value = {{volume}}, *am = &value; struct vnode node = {.dev=dev}, *devvp = &node;\n"
    source += identity + "\n}\n" + CASES
    scenarios = ["multi-container-root", "multi-container-file", "multi-volume-root", "file-only-second", "legacy-devid", "different-device", "native-filesystems", "remount-current-id", "restart-same-order", "unknown-mount", "unknown-inode", "unmounting", "no-path-capability", "busy", "iterref-failure"]
    if args.case:
        scenarios = [args.case]
    with tempfile.TemporaryDirectory(prefix="fsid-lookup-") as tmp:
        output = args.keep_dir.resolve() if args.keep_dir else Path(tmp)
        output.mkdir(parents=True, exist_ok=True)
        cfile, binary = output / "fsid-lookup.c", output / "fsid-lookup"
        cfile.write_text(source)
        subprocess.run(["cc", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-fsanitize=address,undefined", str(cfile), "-o", str(binary)], check=True)
        failed = []
        for scenario in scenarios:
            result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=10)
            print(f"{'PASS' if result.returncode == 0 else 'FAIL'} {scenario}: {result.stdout.strip()}")
            if args.keep_dir:
                (output / (scenario + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode != 0:
                if result.stderr:
                    print(result.stderr.strip())
                failed.append(scenario)
        print(f"allocation={args.allocation} scenarios={len(scenarios)} failed={failed}")
        raise SystemExit(bool(failed))


if __name__ == "__main__":
    main()

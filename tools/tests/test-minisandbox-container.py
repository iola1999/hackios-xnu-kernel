#!/usr/bin/env python3
"""Run the production MiniSandbox container handlers with host credential stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#define MAXPATHLEN 1024
#define KERN_SUCCESS 0
#define Z_WAITOK 1
#define Z_ZERO 2
#define PROC_SETTOKEN_NONE 0
#define CRF_MAC_ENFORCE 1
#define P_LEXIT 1
#define PROC_NULL NULL
#define SANDBOX_CONTAINER_VARIABLE "CFFIXED_USER_HOME="
typedef struct cred *kauth_cred_t;
typedef struct proc *proc_t;
struct label { intptr_t value; bool sealed; };
struct cred { struct label *label; unsigned refs; unsigned cr_flags; };
struct proc { int pid, version, refs; bool entitled; int p_lflag; bool locked, transition; kauth_cred_t cred; uintptr_t user_stack; size_t p_argslen; };
struct vnode {};
struct os_refcnt { unsigned count; };
static struct proc processes[3];
static int allocations, allocation_fail, input_error, output_error, retry_update, change_version;
static void *allocate(size_t size) {
    if (allocation_fail) return NULL;
    void *p = calloc(1, size); assert(p); allocations++; return p;
}
static void release(void *p) { assert(p && allocations > 0); allocations--; free(p); }
#define kalloc_type(t, flags) ((t *)allocate(sizeof(t)))
#define kfree_type(t, p) release(p)
static void os_ref_init(struct os_refcnt *r, void *group) { (void)group; r->count = 1; }
static void os_ref_retain(struct os_refcnt *r) { assert(r->count); r->count++; }
static unsigned os_ref_release(struct os_refcnt *r) { assert(r->count); return --r->count; }
static struct label *mac_cred_label(kauth_cred_t c) { return c->label; }
static intptr_t mac_label_get(struct label *l, int slot) { assert(slot == 0); return l->value; }
static void mac_label_set(struct label *l, int slot, intptr_t value) {
    assert(slot == 0 && !l->sealed); l->value = value;
}
static kauth_cred_t posix_cred_get(kauth_cred_t c) { return c; }
static void mac_cred_label_init(kauth_cred_t c) { c->label = calloc(1, sizeof(*c->label)); assert(c->label); }
static void mac_cred_label_associate(kauth_cred_t parent, kauth_cred_t child);
static void kauth_cred_unref(kauth_cred_t *c);
static kauth_cred_t kauth_cred_proc_ref(proc_t p) { p->cred->refs++; return p->cred; }
static int miniamfi_entitlement_boolean(proc_t p, const char *key) {
    assert(!strcmp(key, "com.apple.private.security.container-manager")); return p->entitled ? 0 : -1;
}
static int copyin(user_addr_t source, void *dest, size_t n) {
    if (input_error) return input_error;
    if (!source) return EFAULT;
    memcpy(dest, (void *)source, n); return 0;
}
static int copyout(const void *source, user_addr_t dest, size_t n) {
    if (output_error) return output_error;
    if (!dest) return EFAULT;
    memcpy((void *)dest, source, n); return 0;
}
static int copyinstr(user_addr_t source, void *dest, size_t n, size_t *length) {
    if (!source) return EFAULT;
    size_t size = strnlen((char *)source, n);
    if (size == n) return ENAMETOOLONG;
    *length = size + 1; memcpy(dest, (void *)source, *length); return 0;
}
static proc_t proc_find(int pid) {
    for (unsigned i = 0; i < 3; i++) if (processes[i].pid == pid) { processes[i].refs++; return &processes[i]; }
    return NULL;
}
static void proc_lock(proc_t p) { assert(!p->locked); p->locked = true; }
static void proc_unlock(proc_t p) { assert(p->locked); p->locked = false; }
static int proc_transstart(proc_t p, int locked, int nonblocking) {
    assert(locked && nonblocking && p->locked);
    if (p->transition) return EDEADLK;
    p->transition = true;
    if (change_version) { change_version = 0; p->version++; }
    return 0;
}
static void proc_transend(proc_t p, int locked) { assert(!locked && p->transition); p->transition = false; }
static int proc_pidversion(proc_t p) { return p->version; }
static void proc_rele(proc_t p) { assert(p->refs > 0); p->refs--; }
static bool kauth_cred_proc_update(proc_t p, int action, bool (^derive)(kauth_cred_t, kauth_cred_t));
/* The environment fallback sees no stack in these processes. */
typedef void *task_t;
typedef void *vm_map_t;
typedef void *vm_map_copy_t;
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_map_address_t;
typedef size_t vm_size_t;
#define TASK_NULL NULL
#define VM_MAP_NULL NULL
#define KMA_DATA 1
#define KMA_ZERO 2
#define VM_KERN_MEMORY_BSD 0
#define FALSE false
#define kernel_map NULL
static task_t proc_task(proc_t p) { (void)p; return NULL; }
#define task_reference(t) ((void)(t))
#define task_deallocate(t) ((void)(t))
#define get_task_map_reference(t) (t)
#define vm_map_deallocate(t) ((void)(t))
#define vm_map_round_page(n,m) (n)
#define vm_map_page_mask(m) 0
#define kmem_alloc(...) (-1)
#define kmem_free(...) ((void)0)
#define vm_map_copyin(...) (-1)
#define vm_map_copy_overwrite(...) (-1)
#define vm_map_copy_discard(...) ((void)0)
'''
HARNESS = r'''
static void mac_cred_label_associate(kauth_cred_t parent, kauth_cred_t child) {
    minisandbox_label_associate(parent, child);
}
static void kauth_cred_unref(kauth_cred_t *cp) {
    kauth_cred_t c = *cp; assert(c->refs);
    if (--c->refs == 0) { minisandbox_label_destroy(c->label); free(c->label); free(c); }
    *cp = NULL;
}
static bool kauth_cred_proc_update(proc_t p, int action, bool (^derive)(kauth_cred_t, kauth_cred_t)) {
    assert(action == PROC_SETTOKEN_NONE && p->transition);
    for (;;) {
        kauth_cred_t model = malloc(sizeof(*model)); *model = *p->cred; model->refs = 1;
        assert(derive(p->cred, model)); model->label->sealed = true;
        if (retry_update) { retry_update = 0; kauth_cred_unref(&model); continue; }
        kauth_cred_unref(&p->cred); p->cred = model; return true;
    }
}
struct set_args { uint64_t pid, version; user_addr_t path; };
struct get_args { int64_t pid; uint64_t version; user_addr_t buffer; user_size_t size; uint64_t check_version; };
static int set_path(proc_t caller, proc_t target, const char *path) {
    struct set_args a = { target->pid, target->version, (user_addr_t)path };
    return minisandbox_set_container_path(caller, (user_addr_t)&a);
}
static int get_path(proc_t target, char *buffer, size_t size) {
    struct get_args a = { target->pid, target->version, (user_addr_t)buffer, size, 1 };
    return minisandbox_container_path((user_addr_t)&a);
}
static void expect_path(proc_t target, const char *path) {
    char result[MAXPATHLEN]; assert(get_path(target, result, sizeof(result)) == 0); assert(!strcmp(path, result));
}
static void exec_path(proc_t p, void *attributes, size_t size, int expected) {
    assert(minisandbox_check_exec(p->cred, NULL, 0, NULL, NULL, NULL, NULL, p, attributes, size));
    kauth_cred_t model = calloc(1, sizeof(*model)); model->refs = 1;
    mac_cred_label_init(model); mac_cred_label_associate(p->cred, model);
    int error = minisandbox_update_exec(p->cred, model, p, NULL, 0, NULL, NULL, NULL, NULL, NULL, attributes, size, NULL);
    assert(error == expected);
    if (!error) { model->label->sealed = true; kauth_cred_unref(&p->cred); p->cred = model; }
    else kauth_cred_unref(&model);
}
int main(void) {
    for (unsigned i = 0; i < 3; i++) {
        processes[i].pid = 10 + i; processes[i].version = 100 + i;
        processes[i].cred = calloc(1, sizeof(struct cred)); processes[i].cred->refs = 1;
        mac_cred_label_init(processes[i].cred); processes[i].cred->label->sealed = true;
    }
    proc_t manager = &processes[0], app = &processes[1], child = &processes[2];
    char result[MAXPATHLEN]; manager->entitled = true;
    assert(get_path(app, result, sizeof(result)) == EPERM);
    assert(set_path(app, app, "/container/denied") == EPERM);
    assert(allocations == 0);
    assert(set_path(manager, app, "/container/first") == 0);
    expect_path(app, "/container/first");
    assert(get_path(app, result, 1) == ENAMETOOLONG);
    output_error = EFAULT; assert(get_path(app, result, sizeof(result)) == EFAULT); output_error = 0;
    struct set_args stale = { app->pid, app->version - 1, (user_addr_t)"/bad" };
    assert(minisandbox_set_container_path(manager, (user_addr_t)&stale) == ESRCH);
    struct get_args stale_get = { app->pid, app->version - 1, (user_addr_t)result, sizeof(result), 1 };
    assert(minisandbox_container_path((user_addr_t)&stale_get) == ESRCH);
    stale.pid = UINT64_MAX; assert(minisandbox_set_container_path(manager, (user_addr_t)&stale) == ESRCH);
    input_error = EFAULT; assert(set_path(manager, app, "/bad") == EFAULT); input_error = 0;
    allocation_fail = 1; assert(set_path(manager, app, "/bad") == ENOMEM); allocation_fail = 0;
    assert(set_path(manager, app, NULL) == EFAULT);
    assert(set_path(manager, app, "") == EINVAL);
    assert(set_path(manager, app, "relative") == EINVAL);
    char long_path[MAXPATHLEN + 1]; memset(long_path, '/', sizeof(long_path)); long_path[MAXPATHLEN] = 0;
    assert(set_path(manager, app, long_path) == ENAMETOOLONG);
    app->transition = true; assert(set_path(manager, app, "/bad") == EDEADLK); app->transition = false;
    app->p_lflag = P_LEXIT; assert(set_path(manager, app, "/bad") == ESRCH); app->p_lflag = 0;
    change_version = 1; assert(set_path(manager, app, "/bad") == ESRCH);
    assert(!app->transition && !app->locked);
    expect_path(app, "/container/first");
    /* Fork shares an immutable credential; replacing the parent's path leaves the child unchanged. */
    kauth_cred_unref(&child->cred); child->cred = kauth_cred_proc_ref(app);
    retry_update = 1; assert(set_path(manager, app, "/container/second") == 0);
    expect_path(app, "/container/second"); expect_path(child, "/container/first");
    unsigned char attributes[0x450] = {0};
    uint32_t structure_size = sizeof(attributes); memcpy(attributes + 4, &structure_size, 4);
    assert(!minisandbox_check_exec(child->cred, NULL, 0, NULL, NULL, NULL, NULL, child, NULL, 0));
    assert(!minisandbox_check_exec(child->cred, NULL, 0, NULL, NULL, NULL, NULL, child, attributes, sizeof(attributes)));
    const char *path = "/container/exec"; uint32_t length = (uint32_t)strlen(path);
    memcpy(attributes + 0xc, &length, sizeof(length)); memcpy(attributes + 0x50, path, length + 1);
    allocation_fail = 1; exec_path(child, attributes, sizeof(attributes), ENOMEM); allocation_fail = 0;
    expect_path(child, "/container/first");
    exec_path(child, attributes, sizeof(attributes), 0);
    expect_path(child, path); expect_path(app, "/container/second");
    exec_path(child, attributes, 0x50 + length, EINVAL);
    attributes[0x50] = 'x'; exec_path(child, attributes, sizeof(attributes), EINVAL);
    attributes[0x50] = '/'; attributes[0x50 + length] = 'x'; exec_path(child, attributes, sizeof(attributes), EINVAL);
    exec_path(child, attributes, 8, EINVAL);
    attributes[0] = 1; exec_path(child, attributes, sizeof(attributes), EINVAL);
    expect_path(child, path);
    for (unsigned i = 0; i < 3; i++) { assert(processes[i].refs == 0); kauth_cred_unref(&processes[i].cred); }
    assert(allocations == 0);
    puts("PASS: setter/query, permissions, stale pidversion, copy/allocation errors, fork isolation, exec attributes, update retries and cleanup");
}
'''


def main():
    source = (ROOT / 'xnu/files/bsd/kern/kern_minimac.c').read_text()
    query = source[source.index('static int\nminisandbox_container_path('):source.index('/* Which request numbers')]
    implementation = (ROOT / 'xnu/files/bsd/kern/kern_minisandbox_container.inc').read_text()
    with tempfile.TemporaryDirectory(prefix='minisandbox-container-') as directory:
        cfile, binary = Path(directory) / 'test.c', Path(directory) / 'test'
        cfile.write_text(PRELUDE + implementation + query + HARNESS)
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
            '-fblocks', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
            '-fsanitize=address,undefined', str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()

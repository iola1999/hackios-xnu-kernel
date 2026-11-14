#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#ifndef __APPLE__
typedef uint64_t user_addr_t;
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define MAXPATHLEN 1024
#define KERN_SUCCESS 0
#define Z_WAITOK 1
#define Z_ZERO 2
#define Z_NOFAIL 4
#define P_LEXIT 1
#define PROC_NULL NULL
#define NULLVP NULL
#define VREG 1
#define VDIR 2
#define VCHR 3
#define SANDBOX_SYSCALL_CONTAINER_PATH 4
#define SANDBOX_SYSCALL_SET_CONTAINER_PATH 66
#define KAUTH_VNODE_READ_DATA 2
#define KAUTH_VNODE_WRITE_DATA 4
#define KAUTH_VNODE_SEARCH 8
#define KAUTH_VNODE_ADD_SUBDIRECTORY 16
#define KAUTH_VNODE_DELETE_CHILD 64
typedef uint32_t kauth_action_t;
typedef void *kauth_cred_t;
struct proc {
    int pid, version, p_lflag, refs, ppid, ppidversion;
    uint64_t uniqueid;
    bool alive, entitled, locked, transition;
    unsigned allowed;
    const char *p_comm;
};
typedef struct proc *proc_t;
struct vnode { int type, usecount, iocount; bool available; };
typedef struct vnode *vnode_t;
typedef proc_t vfs_context_t;
struct os_refcnt { atomic_uint count; };
struct host_lock { pthread_mutex_t mutex; };
#define LCK_GRP_DECLARE(name, label) int name
#define LCK_MTX_DECLARE(name, group) struct host_lock name = { PTHREAD_MUTEX_INITIALIZER }
static _Thread_local bool table_locked;
static struct proc processes[5];
static struct proc *running;
static struct vnode directory = { VDIR, 0, 0, true }, file = { VREG, 0, 0, true };
static int allocations, allocation_fail, context_fail, vnode_ref_error, version_change;
static unsigned copyout_calls, copyout_fail_at;
static bool partial_copyout;
static uint64_t minisandbox_seen;
static unsigned checks;
static int set_calls, get_calls;

static void lck_mtx_lock(struct host_lock *lock) {
    assert(!table_locked); assert(!pthread_mutex_lock(&lock->mutex)); table_locked = true;
}
static void lck_mtx_unlock(struct host_lock *lock) {
    assert(table_locked); table_locked = false; assert(!pthread_mutex_unlock(&lock->mutex));
}
static void *allocate(size_t size, unsigned flags) {
    assert(!table_locked);
    if (allocation_fail && !(flags & Z_NOFAIL)) return NULL;
    void *p = calloc(1, size); assert(p); allocations++; return p;
}
static void release_allocation(void *p) {
    assert(!table_locked && p && allocations > 0); allocations--; free(p);
}
#define kalloc_type(t, flags) ((t *)allocate(sizeof(t), flags))
#define kfree_type(t, p) release_allocation(p)
static void os_ref_init(struct os_refcnt *r, void *group) { (void)group; atomic_init(&r->count, 1); }
static void os_ref_retain(struct os_refcnt *r) { assert(atomic_fetch_add(&r->count, 1) > 0); }
static unsigned os_ref_release(struct os_refcnt *r) {
    unsigned old = atomic_fetch_sub(&r->count, 1); assert(old > 0); return old - 1;
}
static void read_random(void *buffer, unsigned n) {
    static unsigned sequence;
    assert(!table_locked && n == 16); sequence++;
    for (unsigned i = 0; i < n; i++) ((uint8_t *)buffer)[i] = (uint8_t)(sequence * 17 + i);
}
static int copyin(user_addr_t source, void *dest, size_t n) {
    assert(!table_locked);
    if (source < 4096) return EFAULT;
    memcpy(dest, (const void *)source, n); return 0;
}
static int copyout(const void *source, user_addr_t dest, size_t n) {
    assert(!table_locked); copyout_calls++;
    if (dest < 4096) return EFAULT;
    if (copyout_calls == copyout_fail_at) {
        if (partial_copyout && n > 1) memcpy((void *)dest, source, n - 1);
        return EFAULT;
    }
    memcpy((void *)dest, source, n); return 0;
}
static int copyinstr(user_addr_t source, void *dest, size_t n, size_t *length) {
    assert(!table_locked);
    if (source < 4096) return EFAULT;
    size_t size = strnlen((const char *)source, n);
    if (size == n) return ENAMETOOLONG;
    *length = size + 1; memcpy(dest, (const void *)source, *length); return 0;
}
static proc_t current_proc(void) { return running; }
static int proc_getpid(proc_t p) { return p->pid; }
static int proc_pidversion(proc_t p) { return p->version; }
static uint64_t proc_uniqueid(proc_t p) { return p->uniqueid; }
static int proc_original_ppid(proc_t p) { return p->ppid; }
static int proc_orig_ppidversion(proc_t p) { return p->ppidversion; }
static proc_t proc_find(int pid) {
    assert(!table_locked);
    for (unsigned i = 0; i < 5; i++) if (processes[i].alive && processes[i].pid == pid) {
        processes[i].refs++; return &processes[i];
    }
    return NULL;
}
static void proc_rele(proc_t p) { assert(!table_locked && p->refs > 0); p->refs--; }
static void proc_lock(proc_t p) { assert(!table_locked && !p->locked); p->locked = true; }
static void proc_unlock(proc_t p) { assert(!table_locked && p->locked); p->locked = false; }
static int proc_transstart(proc_t p, int locked, int nonblocking) {
    assert(!table_locked && locked && nonblocking && p->locked);
    if (p->transition) return EDEADLK;
    p->transition = true;
    if (version_change) { version_change = 0; p->version++; }
    return 0;
}
static void proc_transend(proc_t p, int locked) {
    assert(!table_locked && !locked && p->transition); p->transition = false;
}
static int miniamfi_entitlement_boolean(proc_t p, const char *name) {
    assert(!table_locked && !strcmp(name, "com.apple.private.security.container-manager"));
    return p->entitled ? KERN_SUCCESS : -1;
}
static vfs_context_t vfs_context_create_with_proc(proc_t p) {
    assert(!table_locked); return context_fail ? NULL : p;
}
static int vfs_context_rele(vfs_context_t context) { assert(!table_locked && context); return 0; }
static int vnode_lookup(const char *path, int flags, vnode_t *vnode, vfs_context_t context) {
    assert(!table_locked && !flags && context);
    if (!strcmp(path, "/containers/app")) *vnode = &directory;
    else if (!strcmp(path, "/files/document")) *vnode = &file;
    else return ENOENT;
    (*vnode)->iocount++; return 0;
}
static int vnode_vtype(vnode_t vnode) { assert(!table_locked && vnode->iocount); return vnode->type; }
static int vnode_authorize(vnode_t vnode, vnode_t parent, kauth_action_t rights, vfs_context_t context) {
    assert(!table_locked && vnode->iocount && parent == NULL && context);
    return (rights & ~context->allowed) ? EACCES : 0;
}
static int vnode_ref(vnode_t vnode) {
    assert(!table_locked && vnode->iocount);
    if (vnode_ref_error) return vnode_ref_error;
    vnode->usecount++; return 0;
}
static void vnode_rele(vnode_t vnode) { assert(!table_locked && vnode->usecount > 0); vnode->usecount--; }
static int vnode_getwithref(vnode_t vnode) {
    assert(!table_locked && vnode->usecount > 0);
    if (!vnode->available) return ENOENT;
    vnode->iocount++; return 0;
}
static int vnode_put(vnode_t vnode) {
    assert(!table_locked && vnode->iocount > 0); vnode->iocount--; return 0;
}
static int minisandbox_set_container_path(struct proc *p, user_addr_t arg) {
    assert(p && arg == 123); set_calls++; return EPERM;
}
static int minisandbox_container_path(user_addr_t arg) { assert(arg == 456); get_calls++; return EFAULT; }

/* MINI_SANDBOX_IMPLEMENTATION */

static void check(bool condition) { checks++; assert(condition); }
static void clean(void) {
    for (unsigned i = 0; i < 5; i++) {
        minisandbox_extension_exit(&processes[i]);
        check(processes[i].refs == 0 && !processes[i].locked && !processes[i].transition);
    }
    check(!allocations && !directory.usecount && !directory.iocount && !file.usecount && !file.iocount);
}
static int issue_raw(proc_t issuer, proc_t target, const char *class_name, const char *path,
    uint64_t flags, uint64_t type, char *token) {
    uint64_t slots[7] = {(user_addr_t)class_name, type, (user_addr_t)path, flags,
        (user_addr_t)token, (uint64_t)(int64_t)target->pid, (uint32_t)target->version};
    running = issuer; return minisandbox_policy_syscall(issuer, 5, (user_addr_t)slots);
}
static char *issue_native(proc_t issuer, proc_t target, const char *class_name, const char *path, uint64_t flags) {
    char *token = malloc(0x3ff0); assert(token); memset(token, 0xa5, 0x3ff0);
    int error = issue_raw(issuer, target, class_name, path, flags, 0, token);
    if (error) { free(token); errno = error; return NULL; }
    check(memchr(token, 0, 2048) != NULL && token[0] != 0);
    return token;
}
static int consume_raw(proc_t owner, const char *token, uint64_t size, int64_t *handle) {
    uint64_t slots[3] = {(user_addr_t)token, size, (user_addr_t)handle};
    running = owner; return minisandbox_policy_syscall(owner, 6, (user_addr_t)slots);
}
static int64_t consume_native(proc_t owner, const char *token, int expected) {
    int64_t handle = 0;
    int error = consume_raw(owner, token, strlen(token) + 1, &handle);
    check(error == expected);
    if (error) return -1;
    check(handle > 0); return handle;
}
static int release_raw(proc_t owner, int64_t handle, uint64_t options, const char *path) {
    uint64_t slots[3] = {(uint64_t)handle, options, (user_addr_t)path};
    running = owner; return minisandbox_policy_syscall(owner, 7, (user_addr_t)slots);
}
static void fork_grants(proc_t parent, proc_t child) {
    running = parent; child->ppid = parent->pid; child->ppidversion = parent->version;
    minisandbox_extension_fork(NULL, child);
}

static void issue_contract(proc_t manager, proc_t app, proc_t other) {
    char output[0x3ff0]; memset(output, 0xa5, sizeof(output));
    check(issue_raw(manager, app, "com.apple.sandbox.container", "/containers/app", 0x30000, 0, output) == 0);
    check(!directory.iocount && directory.usecount == 1);
    int64_t handle = consume_native(app, output, 0);
    check(release_raw(app, handle, 0, NULL) == 0);
    check(directory.usecount == 1);
    clean();
    check(issue_raw(manager, app, "com.apple.sandbox.container", "/files/document", 0x30000, 0, output) == ENOTDIR);
    check(issue_raw(other, app, "com.apple.sandbox.container", "/containers/app", 0x30000, 0, output) == EPERM);
    check(issue_raw(manager, app, "unknown.class", "/containers/app", 0x30000, 0, output) == ENOTSUP);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "relative", 0x30000, 0, output) == EINVAL);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/missing", 0x30000, 0, output) == ENOENT);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0, 0, output) == ENOTSUP);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x10000, 0, output) == ENOTSUP);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 1, output) == ENOTSUP);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30002, 0, output) == ENOTSUP);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, NULL) == EFAULT);
    check(issue_raw(manager, app, NULL, "/files/document", 0x30000, 0, output) == EFAULT);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", NULL, 0x30000, 0, output) == EFAULT);
    char long_string[MAXPATHLEN + 1]; memset(long_string, '/', MAXPATHLEN); long_string[MAXPATHLEN] = 0;
    check(issue_raw(manager, app, long_string, "/files/document", 0x30000, 0, output) == ENAMETOOLONG);
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", long_string, 0x30000, 0, output) == ENAMETOOLONG);
    unsigned allowed = other->allowed; other->allowed = KAUTH_VNODE_READ_DATA;
    check(issue_raw(other, app, "com.apple.app-sandbox.read-write", "/files/document", 0x30000, 0, output) == EACCES);
    check(issue_raw(other, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == 0);
    other->allowed = allowed; clean();
    app->alive = false;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == ESRCH);
    app->alive = true; app->p_lflag = P_LEXIT;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == ESRCH);
    app->p_lflag = 0; app->transition = true;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == EDEADLK);
    app->transition = false; version_change = 1;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == ESRCH);
    uint64_t slots[7] = {(user_addr_t)"com.apple.app-sandbox.read", 0, (user_addr_t)"/files/document",
        0x30000, (user_addr_t)output, (uint64_t)app->pid, (uint32_t)app->version - 1};
    check(minisandbox_policy_syscall(manager, 5, (user_addr_t)slots) == ESRCH);
    slots[6] = UINT64_MAX; check(minisandbox_policy_syscall(manager, 5, (user_addr_t)slots) == ESRCH);
    slots[5] = UINT64_MAX; check(minisandbox_policy_syscall(manager, 5, (user_addr_t)slots) == ESRCH);
    context_fail = 1;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == ENOMEM);
    context_fail = 0; vnode_ref_error = EIO;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == EIO);
    vnode_ref_error = 0; allocation_fail = 1;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == ENOMEM);
    allocation_fail = 0;
    copyout_fail_at = copyout_calls + 1; partial_copyout = true;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == EFAULT);
    copyout_fail_at = 0; partial_copyout = false;
    check(!minisandbox_extensions);
    check(consume_native(app, output, EINVAL) == -1);
    errno = 0;
    check(issue_native(manager, app, "com.apple.app-sandbox.read", "/files/document", 0) == NULL && errno == ENOTSUP);
    clean();
}

static void consume_contract(proc_t manager, proc_t app, proc_t other) {
    char *token = issue_native(manager, app, "com.apple.app-sandbox.read-write", "/files/document", 0x30000);
    check(token != NULL);
    manager->alive = false; minisandbox_extension_exit(manager);
    int64_t first = consume_native(app, token, 0);
    check(release_raw(app, first, 0, NULL) == 0);
    check(consume_native(other, token, EPERM) == -1);
    uint64_t old_uniqueid = app->uniqueid; app->uniqueid++;
    check(consume_native(app, token, EPERM) == -1); app->uniqueid = old_uniqueid;
    app->version++; check(consume_native(app, token, ESRCH) == -1); app->version--;
    char *changed = strdup(token); changed[10] ^= 1;
    check(consume_native(app, changed, EINVAL) == -1); free(changed);
    int64_t handle = 777;
    check(consume_raw(app, token, strlen(token), &handle) == EINVAL && handle == -1);
    check(consume_raw(app, token, strlen(token) + 2, &handle) == EINVAL && handle == -1);
    check(consume_raw(app, token, UINT64_MAX, &handle) == EINVAL && handle == -1);
    check(consume_raw(app, NULL, strlen(token) + 1, &handle) == EFAULT && handle == -1);
    check(consume_raw(app, token, strlen(token) + 1, NULL) == EFAULT);
    copyout_fail_at = copyout_calls + 1;
    check(consume_raw(app, token, strlen(token) + 1, &handle) == EFAULT);
    copyout_fail_at = copyout_calls + 2; partial_copyout = true;
    check(consume_raw(app, token, strlen(token) + 1, &handle) == EFAULT);
    copyout_fail_at = 0; partial_copyout = false;
    check(minisandbox_extension_grants == NULL && file.usecount == 1);
    allocation_fail = 1; check(consume_native(app, token, ENOMEM) == -1); allocation_fail = 0;
    file.available = false; check(consume_native(app, token, ENOENT) == -1); file.available = true;
    unsigned allowed = app->allowed; app->allowed = KAUTH_VNODE_READ_DATA;
    int64_t permitted = consume_native(app, token, 0);
    check(permitted > 0);
    check(release_raw(app, permitted, 0, NULL) == 0);
    app->allowed = allowed;
    app->transition = true; check(consume_native(app, token, EDEADLK) == -1); app->transition = false;
    app->p_lflag = P_LEXIT; check(consume_native(app, token, ESRCH) == -1); app->p_lflag = 0;
    int64_t a = consume_native(app, token, 0), b = consume_native(app, token, 0);
    free(token);
    check(release_raw(other, a, 0, NULL) == EINVAL);
    check(release_raw(app, a, 1, NULL) == ENOTSUP);
    check(release_raw(app, a, 0, "/files/document") == ENOTSUP);
    check(release_raw(app, 0, 0, NULL) == EINVAL);
    check(release_raw(app, -1, 0, NULL) == EINVAL);
    check(release_raw(app, a, 0, NULL) == 0);
    check(release_raw(app, a, 0, NULL) == EINVAL);
    check(release_raw(app, b, 0, NULL) == 0);
    check(minisandbox_policy_syscall(app, 80, 1) == ENOTSUP);
    manager->alive = true; manager->uniqueid++; manager->version++;
    clean();
}

static void lifecycle_contract(proc_t manager, proc_t app, proc_t child) {
    char *token = issue_native(manager, app, "com.apple.sandbox.container", "/containers/app", 0x30000);
    int64_t a = consume_native(app, token, 0), b = consume_native(app, token, 0);
    fork_grants(app, child);
    check(consume_native(child, token, EPERM) == -1);
    check(release_raw(child, a, 0, NULL) == 0);
    check(release_raw(app, a, 0, NULL) == 0);
    app->alive = false; minisandbox_extension_exit(app);
    app->alive = true; app->uniqueid++; app->version++;
    check(consume_native(app, token, EINVAL) == -1);
    check(directory.usecount == 1);
    check(release_raw(child, b, 0, NULL) == 0);
    check(directory.usecount == 0);
    free(token); clean();
    token = issue_native(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000);
    a = consume_native(app, token, 0); fork_grants(app, child);
    /* task_wait_to_return invokes exec_complete for spawn before the child enters EL0. */
    minisandbox_extension_exec_complete(child);
    check(release_raw(child, a, 0, NULL) == EINVAL);
    fork_grants(app, child);
    child->alive = false; minisandbox_extension_exit(child); child->alive = true;
    check(release_raw(child, a, 0, NULL) == EINVAL && file.usecount == 1);
    check(release_raw(app, a, 0, NULL) == 0);
    a = consume_native(app, token, 0);
    fork_grants(app, child);
    app->version++; minisandbox_extension_exec_complete(app);
    check(release_raw(app, a, 0, NULL) == EINVAL);
    check(consume_native(app, token, EINVAL) == -1);
    check(release_raw(child, a, 0, NULL) == 0 && file.usecount == 0);
    free(token); clean();
    token = issue_native(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000);
    a = consume_native(app, token, 0);
    minisandbox_extension_exec_complete(app);
    check(release_raw(app, a, 0, NULL) == EINVAL);
    /* The project's xpcproxy elision keeps pidversion; its issued identity remains current. */
    a = consume_native(app, token, 0); check(release_raw(app, a, 0, NULL) == 0);
    running = app; child->ppidversion = app->version - 1;
    minisandbox_extension_fork(NULL, child);
    check(minisandbox_extension_grants == NULL);
    free(token); clean();
}

static void identifier_contract(proc_t manager, proc_t app) {
    char output[0x3ff0];
    uint64_t saved = minisandbox_extension_identifier;
    minisandbox_extension_identifier = INT64_MAX;
    check(issue_raw(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000, 0, output) == EOVERFLOW);
    clean(); minisandbox_extension_identifier = saved;
    char *token = issue_native(manager, app, "com.apple.app-sandbox.read", "/files/document", 0x30000);
    saved = minisandbox_extension_identifier; minisandbox_extension_identifier = INT64_MAX;
    int64_t handle = 777;
    check(consume_raw(app, token, strlen(token) + 1, &handle) == EOVERFLOW && handle == -1);
    check(!minisandbox_extension_grants && file.usecount == 1);
    minisandbox_extension_identifier = saved;
    handle = consume_native(app, token, 0); check(release_raw(app, handle, 0, NULL) == 0);
    free(token); clean();
}

int main(void) {
    for (unsigned i = 0; i < 5; i++) {
        processes[i] = (struct proc){.pid = 10 + (int)i, .version = 100 + (int)i,
            .uniqueid = 1000 + i, .alive = true, .allowed = UINT_MAX, .p_comm = "Fixture"};
    }
    processes[0].entitled = true;
    issue_contract(&processes[0], &processes[1], &processes[2]);
    consume_contract(&processes[0], &processes[1], &processes[2]);
    lifecycle_contract(&processes[0], &processes[1], &processes[3]);
    identifier_contract(&processes[0], &processes[1]);
    check(minisandbox_policy_syscall(&processes[0], 66, 123) == EPERM && set_calls == 1);
    check(minisandbox_policy_syscall(&processes[1], 4, 456) == EFAULT && get_calls == 1);
    int other_calls[] = {-1, 0, 1, 2, 3, 8, 9, 17, 63, 64};
    for (unsigned i = 0; i < sizeof(other_calls) / sizeof(other_calls[0]); i++)
        check(minisandbox_policy_syscall(&processes[0], other_calls[i], 1) == 0);
    check(minisandbox_policy_syscall(&processes[0], 5, 1) == EFAULT);
    check(minisandbox_policy_syscall(&processes[0], 6, 1) == EFAULT);
    check(minisandbox_policy_syscall(&processes[0], 7, 1) == EFAULT);
    clean();
    printf("PASS: %u contract checks; native ABI, vnode/DAC, target identity, copyout rollback, independent fork release, exec and exit cleanup\n", checks);
}

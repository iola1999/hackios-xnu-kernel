#!/usr/bin/env python3
"""Run the production MiniSandbox dispatch and the native request 5 buffer lifecycle."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef __APPLE__
typedef uint64_t user_addr_t;
#endif
struct proc { const char *p_comm; int pid; };
static uint64_t minisandbox_seen;
static int set_calls, get_calls, set_result, get_result;
static struct proc *set_proc;
static user_addr_t set_arg, get_arg;
static int proc_getpid(struct proc *p) { return p->pid; }
static int minisandbox_set_container_path(struct proc *p, user_addr_t arg) {
    set_calls++; set_proc = p; set_arg = arg; return set_result;
}
static int minisandbox_container_path(user_addr_t arg) {
    get_calls++; get_arg = arg; return get_result;
}
'''
HARNESS = r'''
/* iOS 26 libsystem_sandbox request 5: output pointer at +0x20, output capacity 0x3ff0. */
struct issue_args {
    user_addr_t class_name;
    uint64_t kind;
    user_addr_t resource;
    uint64_t flags;
    user_addr_t output;
    int64_t pid;
    uint64_t options;
};
_Static_assert(sizeof(struct issue_args) == 0x38, "request 5 size");
_Static_assert(offsetof(struct issue_args, output) == 0x20, "request 5 token pointer");
static int allocated, released;
static void release_token(void *buffer) {
    released++; free(buffer); errno = EIO;
}
/* A successful native syscall returns this allocation without inspecting its bytes. */
static unsigned char *native_issue(struct proc *p) {
    unsigned char *buffer = malloc(0x3ff0); assert(buffer); allocated++;
    memset(buffer, 0xa5, 0x3ff0); buffer[0x3fef] = 0;
    struct issue_args arg = { .class_name = (user_addr_t)"diagnostic.read",
        .resource = (user_addr_t)"/diagnostic.bundle", .output = (user_addr_t)buffer };
    int error = minisandbox_policy_syscall(p, 5, (user_addr_t)&arg);
    if (error) {
        int saved_errno = error;
        release_token(buffer);
        errno = saved_errno;
        return NULL;
    }
    return buffer;
}
int main(void) {
    struct proc callers[] = {{"SystemFixture", 10}, {"ApplicationFixture", 11}};
    for (unsigned index = 0; index < 2; index++) {
        unsigned char output[32]; memset(output, 0xa5, sizeof(output));
        struct issue_args arg = { .class_name = 1, .resource = 1, .output = (user_addr_t)output,
            .flags = UINT64_MAX, .pid = -1, .options = UINT64_MAX };
        struct issue_args before = arg;
        int error = minisandbox_policy_syscall(&callers[index], 5, (user_addr_t)&arg);
        assert(error == (EXPECT_UNSUPPORTED ? ENOTSUP : 0));
        assert(!memcmp(&before, &arg, sizeof(arg)));
        for (unsigned byte = 0; byte < sizeof(output); byte++) assert(output[byte] == 0xa5);
        assert(minisandbox_policy_syscall(&callers[index], 5, 0) == error);
        assert(minisandbox_policy_syscall(&callers[index], 5, 1) == error);
        assert(set_calls == 0 && get_calls == 0);
        int old_released = released;
        errno = 0;
        unsigned char *token = native_issue(&callers[index]);
        if (EXPECT_UNSUPPORTED) {
            assert(token == NULL && released == old_released + 1 && errno == ENOTSUP);
        } else {
            assert(token && token[0] == 0xa5 && released == old_released);
            release_token(token);
        }
    }
    assert(allocated == 2 && released == 2);
    int errors[] = {0, EPERM, EFAULT, ENAMETOOLONG};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        set_result = errors[i]; get_result = errors[i];
        assert(minisandbox_policy_syscall(&callers[0], SANDBOX_SYSCALL_SET_CONTAINER_PATH, 123) == errors[i]);
        assert(set_proc == &callers[0] && set_arg == 123 && set_calls == (int)i + 1);
        assert(minisandbox_policy_syscall(&callers[1], SANDBOX_SYSCALL_CONTAINER_PATH, 456) == errors[i]);
        assert(get_arg == 456 && get_calls == (int)i + 1);
    }
    int other_calls[] = {-1, 0, 1, 2, 3, 6, 7, 8, 9, 17, 63, 64};
    for (unsigned i = 0; i < sizeof(other_calls) / sizeof(other_calls[0]); i++) {
        assert(other_calls[i] != SANDBOX_SYSCALL_SET_CONTAINER_PATH);
        assert(minisandbox_policy_syscall(&callers[0], other_calls[i], 1) == 0);
    }
    assert(set_calls == 4 && get_calls == 4);
    puts(EXPECT_UNSUPPORTED ? "PASS: unsupported issue, untouched output, native free/NULL/errno, container dispatch" :
        "PASS: ablation reproduces successful issue with unwritten token bytes");
}
'''


def main():
    source = (ROOT / 'xnu/files/bsd/kern/kern_minimac.c').read_text()
    container = (ROOT / 'xnu/files/bsd/kern/kern_minisandbox_container.inc').read_text()
    dispatch = source[source.index('static int\nminisandbox_policy_syscall('):source.index('/*\n * Privileges')]
    names = ['SANDBOX_SYSCALL_CONTAINER_PATH', 'SANDBOX_SYSCALL_SET_CONTAINER_PATH', 'SANDBOX_SYSCALL_EXTENSION_ISSUE']
    definitions = []
    for name in names:
        found = re.search(r'^#define\s+' + name + r'\s+(\S+)', source + '\n' + container, re.MULTILINE)
        assert found, name
        definitions.append('#define ' + name + ' ' + found.group(1) + '\n')
    assert int(definitions[-1].split()[-1], 0) == 5
    guard = re.compile(r'\n\tif \(call == SANDBOX_SYSCALL_EXTENSION_ISSUE\) \{\n\t\treturn ENOTSUP;\n\t\}\n')
    ablation, removed = guard.subn('\n', dispatch)
    assert removed == 1
    with tempfile.TemporaryDirectory(prefix='minisandbox-issue-') as directory:
        for label, code, expected in [('current', dispatch, 1), ('ablation', ablation, 0)]:
            cfile, binary = Path(directory) / (label + '.c'), Path(directory) / label
            cfile.write_text('#define EXPECT_UNSUPPORTED ' + str(expected) + '\n' + ''.join(definitions) +
                             PRELUDE + code + HARNESS)
            subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            print(result.stdout.splitlines()[-1])


if __name__ == '__main__':
    main()

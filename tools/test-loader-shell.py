#!/usr/bin/env python3
"""Host test of "oem sh": the loader's write into a running kernel's command mailbox.

struct kernel_live, the KERNEL_LIVE_MAGIC* constants and oem_shell() are taken verbatim from
xnuloader/ and compiled against stubs of print() and fb_reply(). The kernel's side of the same
layout is struct loader_live in xnu/files/osfmk/arm64/loader_return.c; the check at the end
compares the offsets the two declare.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = (ROOT / 'xnuloader/loader_02_platform_cpu_ufs_survey.inc').read_text()
USB = (ROOT / 'xnuloader/loader_06_usb_entry.inc').read_text()
KERNEL = (ROOT / 'xnu/files/osfmk/arm64/loader_return.c').read_text()


def function(source, name):
    match = re.search(r'static[^;{}]*\n' + name + r'\([^;]*?\)\n\{', source)
    assert match, name
    level, pos = 1, match.end()
    while level:
        level += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


def block(source, pattern):
    match = re.search(pattern, source, re.S)
    assert match, pattern
    return match.group(0) + '\n'


PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
static unsigned checks;
#define CK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static char log_buf[256], answer[160], info[160];
static uint32_t log_len;
static void print(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_len += (uint32_t)vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, "%llu", ap);
    va_end(ap);
    (void)format;
}
static void fb_reply(const char *status, const char *message)
{
    if (!strcmp(status, "INFO")) snprintf(info, sizeof(info), "%s", message);
    else snprintf(answer, sizeof(answer), "%s%s", status, message);
}
'''

SCENARIOS = r'''
static const struct kernel_live *running_live;
int main(void)
{
    static struct kernel_live live;
    char long_text[200];

    CK(offsetof(struct kernel_live, magic3) == OFF_MAGIC3 && offsetof(struct kernel_live, cmd_seq) == OFF_SEQ &&
       offsetof(struct kernel_live, cmd) == OFF_CMD && sizeof(live.cmd) == CMD_BYTES);
    running_live = NULL;
    oem_shell("ps"); CK(!strcmp(answer, "FAILno kernel is running"));
    running_live = &live;
    live.magic2 = KERNEL_LIVE_MAGIC2;
    oem_shell("ps"); CK(!strcmp(answer, "FAILthis kernel has no command mailbox") && live.cmd_seq == 0);
    live.magic3 = KERNEL_LIVE_MAGIC3;
    oem_shell("ps"); CK(!strcmp(answer, "OKAY1") && !strcmp(info, "1") && live.cmd_seq == 1 && !strcmp(live.cmd, "ps"));
    oem_shell("threads 30"); CK(!strcmp(answer, "OKAY2") && !strcmp(info, "2") && !strcmp(live.cmd, "threads 30"));
    CK(log_len == 0);
    memset(long_text, 'a', 119); long_text[119] = 0;
    oem_shell(long_text); CK(!strcmp(answer, "OKAY3") && strlen(live.cmd) == 119);
    memset(long_text, 'b', 120); long_text[120] = 0;
    oem_shell(long_text); CK(!strncmp(answer, "FAIL", 4) && live.cmd_seq == 3 && live.cmd[0] == 'a');
    printf("test-loader-shell: %u checks passed\n", checks);
    return 0;
}
'''


def kernel_offsets():
    """Byte offsets of magic3, cmd_seq and cmd in the kernel's struct loader_live, and cmd's size.
    Every member before them is 8 bytes (uint64_t or a pointer), so counting declarations is enough."""
    body = block(KERNEL, r'struct loader_live \{.*?\};')
    members = [m for m in re.findall(r'^\s+([^/\n]*?);', body, re.M)]
    offset, offsets = 0, {}
    for member in members:
        names = [n.strip() for n in member.split(' ', 1)[1].split(',')] if ',' in member else [member.split()[-1]]
        for name in names:
            name = name.lstrip('*')
            size = re.match(r'(\w+)\[(\d+)\]', name)
            key = size.group(1) if size else name
            offsets[key] = offset
            offset += int(size.group(2)) if size else 8
    cmd_bytes = int(re.search(r'cmd\[(\d+)\]', body).group(1))
    return offsets['magic3'], offsets['cmd_seq'], offsets['cmd'], cmd_bytes


def main():
    magic3, seq, cmd, cmd_bytes = kernel_offsets()
    source = (PRELUDE + block(PLATFORM, r'struct kernel_live \{.*?\};') +
              ''.join(re.findall(r'#define KERNEL_LIVE_MAGIC\d?\s+0x[0-9a-fA-F]+ULL\n', PLATFORM)) +
              f'#define OFF_MAGIC3 {magic3}\n#define OFF_SEQ {seq}\n#define OFF_CMD {cmd}\n#define CMD_BYTES {cmd_bytes}\n' +
              'static const struct kernel_live *running_live;\n' +
              function(USB, 'oem_shell') + SCENARIOS.replace('static const struct kernel_live *running_live;\n', '', 1))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'shell.c'
        path.write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(path),
                        '-o', str(Path(tmp) / 'shell')], check=True)
        return subprocess.run([str(Path(tmp) / 'shell')]).returncode


if __name__ == '__main__':
    sys.exit(main())

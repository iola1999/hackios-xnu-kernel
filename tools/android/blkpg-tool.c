typedef unsigned long u64;
typedef long i64;
typedef int i32;

#define AT_FDCWD (-100)
#define O_RDWR 2

#define BLKPG 0x1269
#define BLKPG_ADD_PARTITION 1
#define BLKPG_DEL_PARTITION 2
#define BLKPG_RESIZE_PARTITION 3

struct blkpg_ioctl_arg {
    i32 op;
    i32 flags;
    i32 datalen;
    i32 pad;
    void *data;
};

struct blkpg_partition {
    i64 start;
    i64 length;
    i32 pno;
    char devname[64];
    char volname[64];
};

static i64 sys(i64 n, i64 a, i64 b, i64 c, i64 d) {
    register i64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    __asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

static void say(const char *s) {
    i64 n = 0;
    while (s[n]) n++;
    sys(64, 1, (i64)s, n, 0);
}

static void say_num(i64 v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    if (v < 0) {
        say("-");
        v = -v;
    }
    do {
        b[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    say(b + i);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        say("usage: blkpg-tool add DEV PNO START LENGTH\n");
        say("       blkpg-tool resize DEV PNO START LENGTH\n");
        return 1;
    }
    // Simple command handling
    const char *cmd = argv[1];
    const char *dev = argv[2];
    i64 fd = sys(56, AT_FDCWD, (i64)dev, O_RDWR, 0);
    if (fd < 0) {
        say("cannot open "); say(dev); say(": errno "); say_num(-fd); say("\n");
        return 1;
    }

    int op = 0;
    if (cmd[0] == 'a') op = BLKPG_ADD_PARTITION;
    else if (cmd[0] == 'r') op = BLKPG_RESIZE_PARTITION;
    else if (cmd[0] == 'd') op = BLKPG_DEL_PARTITION;

    struct blkpg_partition bp;
    for (int i = 0; i < (int)sizeof(bp); i++) ((char*)&bp)[i] = 0;

    // parse pno
    i64 pno = 0;
    for (const char *p = argv[3]; *p; p++) pno = pno * 10 + (*p - '0');
    bp.pno = (int)pno;

    if (op != BLKPG_DEL_PARTITION) {
        i64 start = 0;
        for (const char *p = argv[4]; *p; p++) start = start * 10 + (*p - '0');
        bp.start = start;

        i64 length = 0;
        for (const char *p = argv[5]; *p; p++) length = length * 10 + (*p - '0');
        bp.length = length;
    }

    struct blkpg_ioctl_arg arg;
    arg.op = op;
    arg.flags = 0;
    arg.datalen = sizeof(bp);
    arg.pad = 0;
    arg.data = &bp;

    i64 r = sys(29, fd, BLKPG, (i64)&arg, 0);
    sys(57, fd, 0, 0, 0);

    say("blkpg: op="); say_num(op); say(" pno="); say_num(pno);
    say(" res="); say_num(r); say("\n");
    return r == 0 ? 0 : 1;
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile (
        "ldr x0, [sp]\n"
        "add x1, sp, #8\n"
        "bl main\n"
        "mov x8, #93\n"
        "svc 0\n"
    );
}

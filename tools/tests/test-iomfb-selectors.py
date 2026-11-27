#!/usr/bin/env python3
"""Host ASan+UBSan test for G8 IOMFB selectors (18/78/88) and S6 l10r_to_bgra conversion."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "xnu/files/iokit/Kernel"


def cut_func(text: str, marker: str) -> str:
    start = text.index(marker)
    open_b = text.index("{", start)
    depth = 0
    for i in range(open_b, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise ValueError(marker)


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define kIOReturnSuccess 0
#define kIOReturnBadArgument (-536870206)
#define kFBNotifyTypes 8
#define kFBNotifyVSync 5
#define kFBColorID 1
#define kFBTimingID 2
#define kFBDotsPerInch 460
#define kFBDefaultWidth 1125
#define kIOReturnNoMemory (-536870211)
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum {
    kFBMethodGetLayerDefaultSurface = 3,
    kFBMethodSwapBegin = 4,
    kFBMethodSwapEnd = 5,
    kFBMethodSwapWait = 6,
    kFBMethodGetID = 7,
    kFBMethodGetDisplaySize = 8,
    kFBMethodRequestPowerChange = 12,
    kFBMethodIsMainDisplay = 18,
    kFBMethodSwapSignal = 20,
    kFBMethodGetDigitalOutState = 25,
    kFBMethodGetDotPitch = 28,
    kFBMethodGetDisplayArea = 29,
    kFBMethodSwapCancel = 52,
    kFBMethodGetColorRemapMode = 57,
    kFBMethodNotifications = 72,
    kFBMethodChangeFrameInfo = 73,
    kFBMethodSupportedFrameInfo = 74,
    kFBMethodGetCurrentAbsoluteTime = 77,
    kFBMethodSetBlock = 78,
    kFBMethodGetBlock = 79,
    kFBMethodSwapCancelAll = 81,
    kFBMethodGetProtectionOptions = 82,
    kFBMethodSetDisplayType = 88,
};

typedef int IOReturn;
typedef struct { int unused; } IOLock;
typedef struct { int unused; } MiniIOSurface;
typedef struct { int unused; } IOExternalMethodDispatch;
typedef struct { int unused; } OSObject;

typedef struct {
    const uint64_t *scalarInput;
    uint32_t scalarInputCount;
    const void *structureInput;
    uint32_t structureInputSize;
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
    void *structureOutput;
    uint32_t structureOutputSize;
} IOExternalMethodArguments;

static uint64_t gFBLogged[2];
static uint8_t gFBCalls[128];
static uint32_t gFBTrace = 0;
static uint32_t gFBPitch = 460;
static bool gTouchLat = false;
static unsigned gNotImplLogs = 0;

static void IOLockLock(IOLock *l) { (void)l; }
static void IOLockUnlock(IOLock *l) { (void)l; }
static uint64_t mach_absolute_time(void) { return 12345678ULL; }
static uint64_t micros(uint64_t t) { return t; }
static void proc_selfname(char *b, int n) { snprintf(b, (size_t)n, "test"); }
static int proc_selfpid(void) { return 1; }
static int proc_pid(void *p) { (void)p; return 1; }
static void *current_proc(void) { return NULL; }
static void *current_thread(void) { return NULL; }
static uint64_t thread_tid(void *t) { (void)t; return 1; }
static int32_t OSIncrementAtomic(volatile int32_t *v) { return (*v)++; }
static uint64_t MiniIOSurfaceMakeSendRight(void *t, MiniIOSurface *s) { (void)t; (void)s; return 1; }
static const uint8_t *miniFramebufferSwapPayload(const void *p, uint32_t n) { (void)p; (void)n; return NULL; }
#define annibale_tl(...) ((void)0)
#define IOLog(...) do { gNotImplLogs++; } while (0)

struct FakeOwner {
    uint32_t fWidth, fHeight;
    volatile int32_t fSwapID;
    MiniIOSurface *defaultSurface(void) { return NULL; }
    void present(const uint8_t *s) { (void)s; }
};

struct IOMobileFramebufferUserClient {
    void *fTask;
    FakeOwner *fOwner;
    IOLock *fLock;
    uint64_t fCallbacks[kFBNotifyTypes];
    uint64_t fRefcons[kFBNotifyTypes];
    bool fVSyncRunning, fClosed;
    uint32_t fDisplayType;
    void armVSync(void) {}
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference);
};

__L10R__

IOReturn
__EXTMETHOD__

int
main(void)
{
    FakeOwner owner = { .fWidth = 1206, .fHeight = 2622 };
    IOLock lock = { 0 };
    IOMobileFramebufferUserClient uc = { .fOwner = &owner, .fLock = &lock, .fDisplayType = 0xdead };

    /* Selector 18: IsMainDisplay returns 1, rejects 0 scalarOutputCount. */
    uint64_t out[2] = { 99, 99 };
    IOExternalMethodArguments a = { .scalarOutput = out, .scalarOutputCount = 1 };
    assert(uc.externalMethod(kFBMethodIsMainDisplay, &a, NULL, NULL, NULL) == kIOReturnSuccess);
    assert(a.scalarOutputCount == 1 && out[0] == 1);
    a.scalarOutputCount = 0;
    assert(uc.externalMethod(kFBMethodIsMainDisplay, &a, NULL, NULL, NULL) == kIOReturnBadArgument);

    /* Selector 88: SetDisplayType stores displayType, rejects missing scalar. */
    uint64_t in88[1] = { 0 };
    IOExternalMethodArguments a88 = { .scalarInput = in88, .scalarInputCount = 1 };
    assert(uc.externalMethod(kFBMethodSetDisplayType, &a88, NULL, NULL, NULL) == kIOReturnSuccess);
    assert(uc.fDisplayType == 0);
    in88[0] = 3;
    assert(uc.externalMethod(kFBMethodSetDisplayType, &a88, NULL, NULL, NULL) == kIOReturnSuccess);
    assert(uc.fDisplayType == 3);
    a88.scalarInputCount = 0;
    assert(uc.externalMethod(kFBMethodSetDisplayType, &a88, NULL, NULL, NULL) == kIOReturnBadArgument);

    /* Selector 78: SetBlock accepts >=2 scalars without logging not implemented, rejects <2. */
    uint64_t in78[4] = { 0x32, 0, 1, 4 };
    uint8_t blk[64] = { 0 };
    IOExternalMethodArguments a78 = { .scalarInput = in78, .scalarInputCount = 4, .structureInput = blk, .structureInputSize = 64 };
    assert(uc.externalMethod(kFBMethodSetBlock, &a78, NULL, NULL, NULL) == kIOReturnSuccess);
    assert(gNotImplLogs == 0);
    a78.scalarInputCount = 1;
    assert(uc.externalMethod(kFBMethodSetBlock, &a78, NULL, NULL, NULL) == kIOReturnBadArgument);

    /* S6 l10r_to_bgra conversion check: B[9:0] G[19:10] R[29:20] A[31:30]. */
    assert(l10r_to_bgra(0x00000000u) == 0x00000000u);
    assert(l10r_to_bgra(0xffffffffu) == 0xffffffffu);
    assert(l10r_to_bgra(0x3ffu) == 0x000000ffu);            /* pure blue */
    assert(l10r_to_bgra(0x3ffu << 10) == 0x0000ff00u);      /* pure green */
    assert(l10r_to_bgra(0x3ffu << 20) == 0x00ff0000u);      /* pure red */
    assert(l10r_to_bgra(0x3u << 30) == 0xff000000u);        /* full alpha */
    assert(l10r_to_bgra(0x1u << 30) == 0x55000000u);        /* 1/3 alpha = 85 */
    assert(l10r_to_bgra(0x2u << 30) == 0xaa000000u);        /* 2/3 alpha = 170 */

    printf("test-iomfb-selectors: PASS\n");
    return 0;
}
"""


def main() -> int:
    uc_text = (KERNEL / "MiniFramebufferUserClient.inc").read_text()
    rows_text = (KERNEL / "MiniFramebufferRows.inc").read_text()
    ext = cut_func(uc_text, "IOMobileFramebufferUserClient::externalMethod(")
    l10r = cut_func(rows_text, "static inline uint32_t\nl10r_to_bgra(")
    code = HARNESS.replace("__EXTMETHOD__", ext).replace("__L10R__", l10r)
    with tempfile.TemporaryDirectory(prefix="iomfb-sel-") as td:
        src = Path(td) / "test.cpp"
        exe = Path(td) / "test"
        src.write_text(code)
        subprocess.run(
            [
                "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-parameter",
                "-fsanitize=address,undefined",
                str(src), "-o", str(exe),
            ],
            check=True,
        )
        return subprocess.run([str(exe)], check=True).returncode


if __name__ == "__main__":
    raise SystemExit(main())

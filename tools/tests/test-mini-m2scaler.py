#!/usr/bin/env python3
"""Host ASan/UBSan test for MiniM2Scaler (AppleM2ScalerCSCDriver)."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
M2_CPP = os.path.join(ROOT, "xnu/files/iokit/Kernel/MiniM2Scaler.cpp")

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef int IOReturn;
typedef uint32_t UInt32;
typedef uint32_t mach_port_t;
typedef uint32_t mach_port_name_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t io_user_reference_t;
typedef io_user_reference_t OSAsyncReference64[8];
typedef struct task * task_t;

#define MACH_PORT_NULL 0u
#define kIOReturnSuccess 0
#define kIOReturnVMError (-536870200)
#define kIOReturnBadArgument (-536870206)
#define kIOReturnUnsupported (-536870201)
#define kIOReturnAborted (-536870165)
#define kIOUserClientClassKey "IOUserClientClass"
#define APPLE_KEXT_OVERRIDE
#ifndef __unused
#define __unused __attribute__((unused))
#endif

static void IOLog(const char *fmt, ...) { (void)fmt; }
static bool PE_parse_boot_argn(const char *, void *, int) { return false; }
extern "C" void proc_selfname(char *buf, int size) { snprintf(buf, (size_t)size, "test"); }
static int copyout(const void *kaddr, user_addr_t uaddr, size_t len) {
    memcpy((void *)(uintptr_t)uaddr, kaddr, len);
    return 0;
}

struct IOLock { int dummy; };
static IOLock * IOLockAlloc(void) { return new IOLock{0}; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *) {}
static void IOLockUnlock(IOLock *) {}

class OSObject {
public:
    int refcount = 1;
    virtual ~OSObject() {}
    virtual bool init() { return true; }
    virtual void free() { delete this; }
    void retain() { refcount++; }
    void release() { if (--refcount == 0) free(); }
};

#define OSDeclareDefaultStructors(cls) public: cls() = default; virtual ~cls() = default
#define OSDefineMetaClassAndStructors(cls, super)
#define OSTypeAlloc(cls) (new cls())
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)

class OSNumber : public OSObject {
public:
    uint64_t val = 0;
    static OSNumber * withNumber(uint64_t v, unsigned) {
        auto * n = new OSNumber();
        n->val = v;
        return n;
    }
};

struct KVPair { char key[96]; OSObject * val; };
class OSDictionary : public OSObject {
public:
    KVPair items[64];
    unsigned count = 0;
    static OSDictionary * withCapacity(unsigned) { return new OSDictionary(); }
    virtual void free() override {
        for (unsigned i = 0; i < count; i++) if (items[i].val) items[i].val->release();
        OSObject::free();
    }
    void setObject(const char * k, OSObject * v) {
        v->retain();
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(items[i].key, k) == 0) {
                items[i].val->release();
                items[i].val = v;
                return;
            }
        }
        assert(count < 64);
        snprintf(items[count].key, sizeof(items[count].key), "%s", k);
        items[count].val = v;
        count++;
    }
    OSObject * getObject(const char * k) const {
        for (unsigned i = 0; i < count; i++)
            if (strcmp(items[i].key, k) == 0) return items[i].val;
        return nullptr;
    }
};

static OSDictionary * gLastCaps;

class IOService : public OSObject {
public:
    OSDictionary * props = OSDictionary::withCapacity(8);
    char userClientClass[64] = "";
    bool registered = false;
    virtual void free() override { OSSafeReleaseNULL(props); OSObject::free(); }
    virtual bool start(IOService *) { return true; }
    bool attach(IOService *) { return true; }
    bool isInactive() const { return false; }
    void terminate() {}
    void registerService() { registered = true; }
    void setProperty(const char * k, const char * s) {
        if (strcmp(k, kIOUserClientClassKey) == 0)
            snprintf(userClientClass, sizeof(userClientClass), "%s", s);
    }
    void setProperty(const char * k, OSObject * o) {
        props->setObject(k, o);
        if (strcmp(k, "IOSurfaceAcceleratorCapabilitiesDict") == 0) {
            o->retain();
            gLastCaps = OSDynamicCast(OSDictionary, o);
        }
    }
};

struct IOExternalMethodArguments {
    const uint64_t * scalarInput;
    uint32_t scalarInputCount;
    const void * structureInput;
    uint32_t structureInputSize;
    uint64_t * scalarOutput;
    uint32_t scalarOutputCount;
    void * structureOutput;
    uint32_t structureOutputSize;
};
struct IOExternalMethodDispatch {};

struct SentAsync {
    mach_port_t port;
    uint64_t callback;
    uint64_t refcon1;
    uint64_t refcon2;
    IOReturn status;
    unsigned count;
};
static SentAsync gLastAsync;

class IOUserClient : public IOService {
public:
    virtual bool initWithTask(task_t, void *, UInt32, OSDictionary *) { return true; }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) {
        return kIOReturnUnsupported;
    }
    static void releaseNotificationPort(mach_port_t) {}
    static void setAsyncReference64(OSAsyncReference64 ref, mach_port_t port, uint64_t fn, uint64_t rc, task_t) {
        memset(ref, 0, sizeof(OSAsyncReference64));
        ref[0] = port;
        ref[1] = fn;
        ref[2] = rc;
    }
    static IOReturn sendAsyncResult64(OSAsyncReference64 ref, IOReturn result, io_user_reference_t args[], UInt32 numArgs) {
        gLastAsync.port = (mach_port_t)ref[0];
        gLastAsync.callback = ref[1];
        gLastAsync.refcon1 = ref[2];
        gLastAsync.refcon2 = numArgs > 0 ? args[0] : 0;
        gLastAsync.status = result;
        gLastAsync.count++;
        return kIOReturnSuccess;
    }
};

class IOBufferMemoryDescriptor : public OSObject {};
class IOMemoryDescriptor : public OSObject {};
class IOMemoryMap : public OSObject {};

#include "MiniIOSurfaceABI.h"

static inline void setNumber(OSDictionary * dict, const char * key, uint64_t value) {
    OSNumber * n = OSNumber::withNumber(value, 32);
    dict->setObject(key, n);
    n->release();
}
void MiniM2ScalerStart(IOService * provider);

struct minisurf_shared {
    uint64_t registryID;
    uint16_t accelID, accelSubID;
    uint32_t seed;
    uint32_t reserved10;
    uint8_t  tiled, reserved15[3];
    uint64_t useCount;
    uint32_t valuesSeed, ycbcrMatrix, valuesSize;
    uint8_t  pixelSizeCasting, displayable, sysMemOnly, reserved2f;
    uint8_t  attachments[0x70];
};

class MiniIOSurface : public OSObject {
public:
    uint32_t sid = 0;
    uint8_t sharedPage[4096] = {};
    uint8_t * kbase = sharedPage;
    uint8_t * kpixels = nullptr;
    struct minisurf_result info = {};
    struct minisurf_plane planes[MINISURF_MAX_PLANES] = {};
    virtual void free() override { free_buf(); OSObject::free(); }
    void free_buf() { if (kpixels) { ::free(kpixels); kpixels = nullptr; } }
    struct minisurf_shared * shared() const { return (struct minisurf_shared *)kbase; }
};

static MiniIOSurface * gSurfs[32];
MiniIOSurface * MiniIOSurfaceCopy(uint32_t sid) {
    if (sid < 32 && gSurfs[sid]) { gSurfs[sid]->retain(); return gSurfs[sid]; }
    return nullptr;
}
void MiniIOSurfaceDrop(MiniIOSurface * s) { if (s) s->release(); }
bool MiniIOSurfaceDrawn(MiniIOSurface *, uint64_t) { return true; }

static uint64_t gEventVals[8];
IOReturn MiniIOSurfaceWaitSharedEvent(task_t, mach_port_name_t p, uint64_t target, uint64_t) {
    return (p < 8 && gEventVals[p] >= target) ? kIOReturnSuccess : -1;
}
IOReturn MiniIOSurfaceSignalSharedEvent(task_t, mach_port_name_t p, uint64_t val) {
    if (p < 8 && val > gEventVals[p]) gEventVals[p] = val;
    return kIOReturnSuccess;
}

#include "m2scaler_body.inc"

static MiniIOSurface * makeBiplanar(uint32_t sid, uint32_t fmt, uint32_t w, uint32_t h, bool wide) {
    auto * s = new MiniIOSurface();
    s->sid = sid;
    s->shared()->seed = 1;
    uint32_t bpe0 = wide ? 2 : 1, bpe1 = wide ? 4 : 2;
    uint32_t bpr0 = ((w * bpe0 + 63) & ~63u);
    uint32_t bpr1 = (((w / 2) * bpe1 + 63) & ~63u);
    uint32_t sz0 = bpr0 * h, sz1 = bpr1 * (h / 2);
    s->info.width = w; s->info.height = h; s->info.pixelFormat = fmt;
    s->info.planeCount = 2; s->info.allocSize = 512 + sz0 + sz1;
    s->planes[0].width = w; s->planes[0].height = h; s->planes[0].bytesPerElement = bpe0;
    s->planes[0].bytesPerRow = bpr0; s->planes[0].offset = 512; s->planes[0].size = sz0;
    s->planes[1].width = w / 2; s->planes[1].height = h / 2; s->planes[1].bytesPerElement = bpe1;
    s->planes[1].bytesPerRow = bpr1; s->planes[1].offset = 512 + sz0; s->planes[1].size = sz1;
    s->kpixels = (uint8_t *)calloc(1, s->info.allocSize);
    gSurfs[sid] = s;
    return s;
}

static MiniIOSurface * makeSingle(uint32_t sid, uint32_t fmt, uint32_t w, uint32_t h, uint32_t bpe) {
    auto * s = new MiniIOSurface();
    s->sid = sid;
    s->shared()->seed = 1;
    uint32_t bpr = ((w * bpe + 63) & ~63u);
    s->info.width = w; s->info.height = h; s->info.pixelFormat = fmt;
    s->info.bytesPerElement = bpe; s->info.bytesPerRow = bpr;
    s->info.planeCount = 0; s->info.offset = 128; s->info.allocSize = 128 + bpr * h;
    s->planes[0].width = w; s->planes[0].height = h; s->planes[0].bytesPerElement = bpe;
    s->planes[0].bytesPerRow = bpr; s->planes[0].offset = 0; s->planes[0].size = bpr * h;
    s->kpixels = (uint8_t *)calloc(1, s->info.allocSize);
    gSurfs[sid] = s;
    return s;
}

int main(void) {
    /* 1. Capabilities check (AppleJPEG + CMPhoto + CMImaging FigM2MController + VideoToolbox) */
    auto * prov = new IOService();
    MiniM2ScalerStart(prov);
    assert(gLastCaps != nullptr && gLastCaps->count == 40);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterVerticalTapsCount"))->val == 4);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterHorizontalTapsCount"))->val == 8);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterVerticalPhasesCount"))->val == 16);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterHorizontalPhasesCount"))->val == 16);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterCoefficientsPrePointBits"))->val == 2);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFilterCoefficientsPostPointBits"))->val == 14);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorCapabilitiesSymmetricScaling"))->val == 1);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("kSurfaceAcceleratorCapabilitiesHistogramValues"))->val == 128);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorFormatIn2Planes420"))->val == 1);
    assert(OSDynamicCast(OSNumber, gLastCaps->getObject("IOSurfaceAcceleratorScalerVersion"))->val == 0x40007);
    gLastCaps->release();
    gLastCaps = nullptr;
    prov->release();

    auto * uc = new AppleM2ScalerCSCDriverUserClient();
    assert(uc->initWithTask((task_t)0x1234, nullptr, 0, nullptr));
    assert(uc->start(nullptr));
    assert(uc->registerNotificationPort(77, 0, 0) == kIOReturnSuccess);

    /* 2. P010 ('xf20') -> NV12 ('420f') downscale + async callback + SharedEvent signal */
    auto * srcP010 = makeBiplanar(1, FOURCC('x','f','2','0'), 8, 8, true);
    auto * dstNV12 = makeBiplanar(2, FOURCC('4','2','0','f'), 4, 4, false);
    uint16_t * sy = (uint16_t *)(srcP010->kpixels + srcP010->planes[0].offset);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++)
            sy[y * (srcP010->planes[0].bytesPerRow / 2) + x] = (uint16_t)(200 << 8);
    uint16_t * suv = (uint16_t *)(srcP010->kpixels + srcP010->planes[1].offset);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++) {
            suv[y * (srcP010->planes[1].bytesPerRow / 2) + x * 2] = (uint16_t)(96 << 8);
            suv[y * (srcP010->planes[1].bytesPerRow / 2) + x * 2 + 1] = (uint16_t)(160 << 8);
        }

    uint8_t rawReq[0x1b0] = {};
    auto * req = (struct m2scaler_req *)rawReq;
    req->srcSurfaceID = 1;
    req->dstSurfaceID = 2;
    req->callback = 0xdeadbeefULL;
    req->refcon1 = 0x1111ULL;
    req->refcon2 = 0x2222ULL;
    req->dstEventPort = 3;
    req->dstEventSignal = 42;
    req->histBinMode = 1;

    IOExternalMethodArguments args = {};
    args.structureInput = rawReq;
    args.structureInputSize = sizeof(rawReq);
    assert(uc->externalMethod(kM2MethodSetProperty, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(uc->externalMethod(kM2MethodSetCustomFilter, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);

    uint32_t histOut[3][128] = {};
    uint64_t histPtr = (uint64_t)(uintptr_t)histOut;
    IOExternalMethodArguments hargs = {};
    hargs.structureInput = &histPtr;
    hargs.structureInputSize = sizeof(histPtr);
    assert(uc->externalMethod(kM2MethodGetHistogram, &hargs, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(histOut[0][200 >> 1] == 16);
    assert(histOut[1][96 >> 1] == 16);
    assert(histOut[2][160 >> 1] == 16);

    assert(dstNV12->shared()->seed == 2);
    assert(gEventVals[3] == 42);
    assert(gLastAsync.count == 1 && gLastAsync.port == 77);
    assert(gLastAsync.callback == 0xdeadbeefULL && gLastAsync.refcon1 == 0x1111ULL && gLastAsync.refcon2 == 0x2222ULL);
    assert(dstNV12->kpixels[dstNV12->planes[0].offset] == 200);
    assert(dstNV12->kpixels[dstNV12->planes[1].offset] == 96);
    assert(dstNV12->kpixels[dstNV12->planes[1].offset + 1] == 160);

    /* 3. SkipAsyncCallback + NV12 -> BGRA CSC */
    auto * dstBGRA = makeSingle(3, FOURCC('B','G','R','A'), 4, 4, 4);
    memset(rawReq, 0, sizeof(rawReq));
    req->srcSurfaceID = 2;
    req->dstSurfaceID = 3;
    req->callback = 0xdeadbeefULL;
    req->flags = kM2FlagSkipAsyncCallback | kM2FlagAlphaOverride;
    req->alphaOverride = 0x7f;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(gLastAsync.count == 1); /* skipped! */
    const uint8_t * bgra = dstBGRA->kpixels + dstBGRA->info.offset;
    assert(bgra[3] == 0x7f && bgra[2] > 200 && bgra[0] < 200);

    /* 4. L00h half-float 1.0 (0x3c00) scale + ZeroFill */
    auto * srcHalf = makeSingle(4, FOURCC('L','0','0','h'), 4, 4, 2);
    auto * dstHalf = makeSingle(5, FOURCC('L','0','0','h'), 2, 2, 2);
    uint16_t * hp = (uint16_t *)(srcHalf->kpixels + srcHalf->info.offset);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++)
            hp[y * (srcHalf->info.bytesPerRow / 2) + x] = 0x3c00u;
    memset(rawReq, 0, sizeof(rawReq));
    req->srcSurfaceID = 4;
    req->dstSurfaceID = 5;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(((uint16_t *)(dstHalf->kpixels + dstHalf->info.offset))[0] == 0x3c00u);

    req->flags = kM2FlagZeroFill;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    assert(((uint16_t *)(dstHalf->kpixels + dstHalf->info.offset))[0] == 0u);

    /* 5. CW 90° (kM2FlagRotate90 = 0x4) orientation check on both BGRA general path and 420f fast path */
    auto * srcRotBGRA = makeSingle(6, FOURCC('B','G','R','A'), 4, 2, 4);
    auto * dstRotBGRA = makeSingle(7, FOURCC('B','G','R','A'), 2, 4, 4);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 4; x++) {
            uint8_t * p = srcRotBGRA->kpixels + srcRotBGRA->info.offset + y * srcRotBGRA->info.bytesPerRow + x * 4;
            p[0] = (uint8_t)(y * 16 + x + 1);
            p[1] = 0x20;
            p[2] = 0x40;
            p[3] = 0xff;
        }
    }
    memset(rawReq, 0, sizeof(rawReq));
    req->srcSurfaceID = 6;
    req->dstSurfaceID = 7;
    req->flags = kM2FlagRotate90 | kM2FlagSkipAsyncCallback;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    const uint8_t * dr = dstRotBGRA->kpixels + dstRotBGRA->info.offset;
    /* CW 90°: src(0,0)=1 -> dst(1,0); src(0,1)=17 -> dst(0,0); src(3,0)=4 -> dst(1,3). */
    assert(dr[0 * dstRotBGRA->info.bytesPerRow + 1 * 4] == 1);
    assert(dr[0 * dstRotBGRA->info.bytesPerRow + 0 * 4] == 17);
    assert(dr[3 * dstRotBGRA->info.bytesPerRow + 1 * 4] == 4);
    assert(uc->externalMethod(kM2MethodAbortTransfers, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);

    /* 6. '444f' (4:4:4 biplanar, AppleJPEG's output for a 4:4:4 JPEG) -> RGBA: one CbCr pair per pixel */
    auto * src444 = makeBiplanar(8, FOURCC('4','4','4','f'), 4, 2, false);
    src444->planes[1].width = 4; src444->planes[1].height = 2; src444->planes[1].bytesPerRow = 64;
    src444->planes[1].size = 64 * 2;
    free(src444->kpixels);
    src444->info.allocSize = 512 + src444->planes[0].size + src444->planes[1].size;
    src444->kpixels = (uint8_t *)calloc(1, src444->info.allocSize);
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 4; x++) {
            src444->kpixels[src444->planes[0].offset + y * src444->planes[0].bytesPerRow + x] = (uint8_t)(40 + 50 * x + 20 * y);
            src444->kpixels[src444->planes[1].offset + y * 64 + x * 2] = 128;
            src444->kpixels[src444->planes[1].offset + y * 64 + x * 2 + 1] = 128;
        }
    auto * dstRGBA = makeSingle(9, FOURCC('R','G','B','A'), 4, 2, 4);
    memset(rawReq, 0, sizeof(rawReq));
    req->srcSurfaceID = 8;
    req->dstSurfaceID = 9;
    req->flags = kM2FlagSkipAsyncCallback;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnSuccess);
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 4; x++) {
            const uint8_t * px = dstRGBA->kpixels + dstRGBA->info.offset + y * dstRGBA->info.bytesPerRow + x * 4;
            uint8_t luma = (uint8_t)(40 + 50 * x + 20 * y);
            assert(px[0] == luma && px[1] == luma && px[2] == luma && px[3] == 255);
        }

    /* 7. A format without texel access ('y420', three planes) is refused, not read as BGRA */
    auto * srcPlanar = makeSingle(10, FOURCC('y','4','2','0'), 4, 2, 1);
    auto * dstRefused = makeSingle(11, FOURCC('B','G','R','A'), 4, 2, 4);
    memset(rawReq, 0, sizeof(rawReq));
    req->srcSurfaceID = 10;
    req->dstSurfaceID = 11;
    req->flags = kM2FlagSkipAsyncCallback;
    assert(uc->externalMethod(kM2MethodTransformSurface, &args, nullptr, nullptr, nullptr) == kIOReturnUnsupported);
    (void)srcPlanar;
    (void)dstRefused;

    assert(uc->clientClose() == kIOReturnSuccess);
    uc->release();
    for (int i = 1; i <= 11; i++) { gSurfs[i]->release(); gSurfs[i] = nullptr; }
    printf("MiniM2Scaler host test: P010->NV12, NV12->BGRA CSC, L00h half-float, ZeroFill, CW90, 444f->RGBA, "
           "unsupported refusal, SharedEvent & AsyncCallback PASS\n");
    return 0;
}
'''


def main() -> int:
    with open(M2_CPP, "r", encoding="utf-8") as f:
        src = f.read()
    marker = "#define FOURCC(a, b, c, d)"
    body = src[src.index(marker):]
    with tempfile.TemporaryDirectory(prefix="test-m2scaler-") as td:
        with open(os.path.join(td, "m2scaler_body.inc"), "w", encoding="utf-8") as f:
            f.write(body)
        harness_cpp = os.path.join(td, "harness.cpp")
        with open(harness_cpp, "w", encoding="utf-8") as f:
            f.write(HARNESS)
        out_bin = os.path.join(td, "test_m2scaler")
        cmd = [
            "clang++", "-std=c++17", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-Wall", "-Wextra", "-Werror",
            "-I", td,
            "-I", os.path.join(ROOT, "xnu/files/iokit/Kernel"),
            harness_cpp, "-o", out_bin,
        ]
        subprocess.run(cmd, check=True)
        res = subprocess.run([out_bin], capture_output=True, text=True)
        if res.returncode != 0:
            sys.stderr.write(res.stderr)
            raise RuntimeError(f"test_m2scaler failed with rc={res.returncode}")
        print(res.stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())

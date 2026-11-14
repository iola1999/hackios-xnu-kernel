#!/usr/bin/env python3
"""Validate IOSurface wire layouts and the driver's mapping ownership on the host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "xnu/files/iokit/Kernel/MiniIOSurfaceABI.h"
DRIVER = ROOT / "xnu/files/iokit/Kernel/MiniIOSurface.cpp"

ABI_SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "MiniIOSurfaceABI.h"

static uint64_t read(const uint8_t *p, unsigned offset, unsigned width)
{
    uint64_t result = 0;
    memcpy(&result, p + offset, width);
    return result;
}

static void test_init(void)
{
    for (unsigned size = 0; size <= 0x29; ++size) {
        uint8_t output[0x30];
        memset(output, 0xa5, sizeof(output));
        bool valid = size == 0x18 || size == 0x28;
        assert(minisurfInitReply(output, size, 0x3fff, 63, 16384) == valid);
        if (!valid) {
            for (uint8_t byte : output) assert(byte == 0xa5);
            continue;
        }
        assert(read(output, 0, 4) == 0x3fff);
        assert(read(output, 4, 4) == 63);
        if (size == 0x28) {
            assert(read(output, 8, 8) == 262144);
            assert(read(output, 0x10, 8) == 16384);
            assert(read(output, 0x18, 8) == 16384);
            assert(read(output, 0x22, 2) == 1);
        } else {
            assert(read(output, 8, 4) == 262144);
            assert(read(output, 0xc, 4) == 16384);
            assert(read(output, 0x10, 4) == 16384);
            assert(read(output, 0x16, 2) == 1);
        }
        assert(output[size] == 0xa5);
    }
    assert(!minisurfInitReply(NULL, 0x28, 0, 0, 0));
}

static void test_result(unsigned count)
{
    minisurf_result info = {};
    info.base = UINT64_MAX;
    info.shared = 0xabcdef1234567890ULL;
    info.aux = 0xabcdef1234567c90ULL;
    info.sid = 0x10203040;
    info.allocSize = 0xfedcba98;
    info.width = 1206; info.height = 2622; info.bytesPerRow = 4864; info.offset = 128;
    info.pixelFormat = 0x42475241; info.planeCount = count;
    info.bytesPerElement = 4; info.elementWidth = 1; info.elementHeight = 2;
    info.cacheMode = 3; info.lockSeed = 17; info.subsampling = 2; info.prohibitUseCount = 1;
    info.parentID = 91; info.sliceCount = 2; info.traceID = 0x1020304050607080ULL;
    info.protectionOptions = 0xfedcba9876543210ULL;
    minisurf_plane planes[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        planes[i].width = 301 + i; planes[i].height = 601 + i;
        planes[i].offset = 0xffff0000 + i; planes[i].bytesPerRow = 2048 + i; planes[i].size = 0xf0000000 + i;
        planes[i].bytesPerElement = 2 + i; planes[i].elementWidth = 1; planes[i].elementHeight = 2;
        planes[i].componentCount = 4; planes[i].addressFormat = 1; planes[i].compression = i;
        for (unsigned j = 0; j < 4; ++j) {
            planes[i].componentBitDepth[j] = 8 + j;
            planes[i].componentBitOffset[j] = 16 + j;
            planes[i].componentName[j] = 24 + j;
            planes[i].componentType[j] = 32 + j;
            planes[i].componentRange[j] = 40 + j;
            planes[i].extendedPixels[j] = 100 * i + j;
        }
    }
    for (minisurf_abi abi : {MINISURF_ABI16, MINISURF_ABI26}) {
        unsigned plane_count = count ? count : 1;
        unsigned expected = abi == MINISURF_ABI26 ? 0x88 + plane_count * 0x98 : 0x68 + plane_count * 0x80;
        uint8_t allocation[0x400];
        uint8_t *output = allocation + 1;
        memset(allocation, 0xa5, sizeof(allocation));
        assert(minisurfWriteResult(output, expected - 1, abi, &info, planes) == 0);
        for (uint8_t byte : allocation) assert(byte == 0xa5);
        assert(minisurfWriteResult(output, expected, abi, &info, planes) == expected);
        assert(allocation[0] == 0xa5 && output[expected] == 0xa5);
        if (abi == MINISURF_ABI16) {
            static_assert(sizeof(minisurf_result) == 0x68, "legacy header");
            static_assert(sizeof(minisurf_plane) == 0x80, "legacy plane");
            assert(!memcmp(output, &info, 0x68));
            assert(!memcmp(output + 0x68, planes, plane_count * 0x80));
            continue;
        }
        struct Field { unsigned offset, width; uint64_t value; } fields[] = {
            {0,8,info.base}, {8,8,info.shared}, {0x10,8,info.aux}, {0x18,4,info.sid},
            {0x20,8,info.allocSize}, {0x28,8,1206}, {0x30,8,2622}, {0x38,8,4864}, {0x40,8,128},
            {0x48,4,0x42475241}, {0x58,4,count}, {0x60,2,4}, {0x62,1,1}, {0x63,1,2},
            {0x64,4,3}, {0x68,4,17}, {0x6c,1,2}, {0x6d,1,1}, {0x70,4,91}, {0x74,4,2},
            {0x78,8,info.traceID}, {0x80,8,info.protectionOptions},
        };
        for (Field field : fields) assert(read(output, field.offset, field.width) == field.value);
        assert(read(output, 0x1c, 4) == 0 && read(output, 0x50, 8) == 0);
        for (unsigned i = 0; i < plane_count; ++i) {
            const uint8_t *p = output + 0x88 + i * 0x98;
            assert(read(p, 0, 8) == 301 + i && read(p, 8, 8) == 601 + i);
            assert(read(p, 0x18, 8) == 0xffff0000 + i);
            assert(read(p, 0x20, 8) == 2048 + i && read(p, 0x28, 8) == 0xf0000000 + i);
            assert(read(p, 0x30, 2) == 2 + i && p[0x32] == 1 && p[0x33] == 2);
            assert(p[0x34] == 4 && p[0x35] == 1 && p[0x6c] == i);
            for (unsigned j = 0; j < 4; ++j) {
                assert(p[0x38+j] == 8+j && p[0x3c+j] == 16+j && p[0x40+j] == 24+j);
                assert(p[0x44+j] == 32+j && p[0x48+j] == 40+j);
                assert(read(p, 0x88+4*j, 4) == 100*i+j);
            }
        }
    }
    info.planeCount = 5;
    uint8_t bytes[1];
    assert(!minisurfWriteResult(bytes, sizeof(bytes), MINISURF_ABI26, &info, planes));
}

static void test_bulk(void)
{
    const unsigned begin[] = {0,0x20,0x30,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x58,0x5c,0x64,0x68,0x70,0x65,0x66,0x74,0x80,0x82};
    const unsigned end[] = {0x20,0x30,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x58,0x5c,0x64,0x65,0x70,0x74,0x66,0x67,0x80,0x82,0x84};
    for (minisurf_abi abi : {MINISURF_ABI16, MINISURF_ABI26}) {
        unsigned suffix = abi == MINISURF_ABI26 ? 0x88 : 0x70;
        uint64_t limit = abi == MINISURF_ABI26 ? 0x84 : 0x70;
        unsigned base = abi == MINISURF_ABI26 ? 0x34 : 0x30;
        unsigned fields = abi == MINISURF_ABI26 ? 22 : 15;
        uint8_t input[0xa0], shared[0xc0];
        memset(input, 0x3c, sizeof(input));
        uint64_t size = 0, mask = 0;
        uint32_t sid = 0, expected_sid = 197;
        memcpy(input + suffix, &limit, 8);
        memcpy(input + suffix + 16, &expected_sid, 4);
        for (unsigned i = 0; i < fields; ++i) {
            uint64_t bit = 1ULL << i;
            memcpy(input + suffix + 8, &bit, 8);
            assert(minisurfBulkInput(abi, input, suffix + 0x18, &size, &mask, &sid));
            assert(size == limit && mask == bit && sid == 197);
            assert(!minisurfBulkInput(abi, input, suffix + 0x17, &size, &mask, &sid));
            memset(shared, 0xa5, sizeof(shared));
            minisurfApplyBulk(shared, abi, input, size, mask);
            for (unsigned j = 0; j < sizeof(shared); ++j)
                assert(shared[j] == (j >= base+begin[i] && j < base+end[i] ? 0x3c : 0xa5));
            memset(shared, 0xa5, sizeof(shared));
            assert(minisurfClearBulk(shared, abi, mask) == (abi == MINISURF_ABI26));
            for (unsigned j = 0; j < sizeof(shared); ++j)
                assert(shared[j] == (abi == MINISURF_ABI26 && j >= base+begin[i] && j < base+end[i] ? 0 : 0xa5));
            memset(shared, 0xa5, sizeof(shared));
            assert(!minisurfClearBulk(shared, abi, 1ULL << 63));
            for (uint8_t byte : shared) assert(byte == 0xa5);
            minisurfApplyBulk(shared, abi, input, end[i] - 1, mask);
            for (uint8_t byte : shared) assert(byte == 0xa5);
        }
        ++limit;
        memcpy(input + suffix, &limit, 8);
        assert(!minisurfBulkInput(abi, input, suffix+0x18, &size, &mask, &sid));
        memset(shared, 0xa5, sizeof(shared));
        minisurfSetYCbCrMatrix(shared, abi, 7);
        if (abi == MINISURF_ABI26) {
            assert(shared[0x6e] == 7 && read(shared, 0x24, 4) == 0xa5a5a5a5);
        } else {
            assert(read(shared, 0x24, 4) == 7 && shared[0x6e] == 0xa5);
        }
    }
}

int main(void)
{
    test_init();
    for (unsigned count : {0u, 1u, 4u}) test_result(count);
    test_bulk();
    puts("IOSurface ABI: init, results, planes, bulk fields and bounds passed");
}
'''

MAP_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include "MiniIOSurfaceABI.h"
typedef int IOReturn;
enum {kIOReturnSuccess, kIOReturnUnsupported, kIOReturnNoMemory, kIOReturnBadArgument, kIOMapAnywhere};
enum {PAGE_MASK = 0x3fff, MINISURF_ROW_ALIGN = 64, MINISURF_MAX_DIMENSION = 16384};
static int gSurfaceLock;
static void IOLockLock(int) {}
static void IOLockUnlock(int) {}
struct IOExternalMethodArguments { void *structureOutput; uint32_t structureOutputSize; };
static int liveMaps;
struct IOMemoryMap {
    IOMemoryMap() { ++liveMaps; }
    void release() { --liveMaps; delete this; }
};
struct Memory {
    bool fail;
    IOMemoryMap *createMappingInTask(int, int, int) { return fail ? NULL : new IOMemoryMap; }
};
struct MiniIOSurface {
    minisurf_abi sharedABI;
    unsigned sid, holders, useCount[2];
    minisurf_result info;
    Memory *memory, *client;
    int clientTask;
    struct Shared { uint64_t useCount; } state;
    Shared *shared() { return &state; }
};
struct minisurf_client_ref {
    MiniIOSurface *surface;
    IOMemoryMap *map, *pixelMap;
    uint32_t use[2];
    bool detached;
};
struct IOSurfaceRootUserClient {
    int fTask;
    minisurf_client_ref *fRefs;
    unsigned fRefCount, fRefCapacity;
    minisurf_abi fABI;
    minisurf_client_ref *addRef(MiniIOSurface *, IOReturn *);
    void dropRef(minisurf_client_ref *);
    IOReturn methodInit(IOExternalMethodArguments *);
};
#define IONew(type, count) new (std::nothrow) type[count]
#define IODelete(pointer, type, count) delete[] pointer
#define IOLog(...) ((void)0)
#define OSSafeReleaseNULL(pointer) do { if (pointer) { pointer->release(); pointer = NULL; } } while (0)
static void surfaceHold(MiniIOSurface *s) { ++s->holders; }
static void surfaceUnhold(MiniIOSurface *s) { --s->holders; }
'''
MAP_MAIN = r'''
int main(void)
{
    for (minisurf_abi abi : {MINISURF_ABI16, MINISURF_ABI26}) {
        Memory memory = {true}, pixels = {true};
        MiniIOSurface surface = {};
        surface.memory = &memory; surface.client = &pixels; surface.clientTask = 2; surface.holders = 1;
        IOSurfaceRootUserClient client = {};
        client.fTask = 1; client.fABI = abi;
        IOReturn error = -1;
        assert(!client.addRef(&surface, &error));
        assert(error == kIOReturnNoMemory && liveMaps == 0 && client.fRefCount == 0);
        assert(surface.sharedABI == MINISURF_UNCLAIMED && surface.holders == 1);
        memory.fail = false;
        assert(!client.addRef(&surface, &error));
        assert(error == kIOReturnNoMemory && liveMaps == 0 && client.fRefCount == 0);
        assert(surface.sharedABI == MINISURF_UNCLAIMED && surface.holders == 1);
        pixels.fail = false;
        minisurf_client_ref *ref = client.addRef(&surface, &error);
        assert(ref && error == kIOReturnSuccess && surface.sharedABI == abi);
        assert(liveMaps == 2 && client.fRefCount == 1 && surface.holders == 2);
        IOSurfaceRootUserClient other = {};
        other.fTask = 3; other.fABI = abi == MINISURF_ABI16 ? MINISURF_ABI26 : MINISURF_ABI16;
        assert(!other.addRef(&surface, &error));
        assert(error == kIOReturnUnsupported && liveMaps == 2 && other.fRefCount == 0 && !other.fRefs);
        client.dropRef(ref);
        assert(liveMaps == 0 && client.fRefCount == 0 && surface.holders == 1);
        assert(!other.addRef(&surface, &error) && error == kIOReturnUnsupported);
        ref = client.addRef(&surface, &error);
        assert(ref && error == kIOReturnSuccess);
        client.dropRef(ref);
        delete[] client.fRefs;
    }
    IOSurfaceRootUserClient negotiated = {};
    uint8_t output[0x28];
    IOExternalMethodArguments args = {output, 0x27};
    assert(negotiated.methodInit(&args) == kIOReturnBadArgument);
    assert(negotiated.fABI == MINISURF_UNCLAIMED);
    args.structureOutputSize = 0x28;
    assert(negotiated.methodInit(&args) == kIOReturnSuccess && negotiated.fABI == MINISURF_ABI26);
    assert(negotiated.methodInit(&args) == kIOReturnSuccess);
    args.structureOutputSize = 0x18;
    assert(negotiated.methodInit(&args) == kIOReturnBadArgument && negotiated.fABI == MINISURF_ABI26);
    IOSurfaceRootUserClient legacy = {};
    Memory memory = {false};
    MiniIOSurface surface = {};
    surface.memory = &memory; surface.holders = 1;
    IOReturn error;
    minisurf_client_ref *ref = legacy.addRef(&surface, &error);
    assert(ref && legacy.fABI == MINISURF_ABI16 && surface.sharedABI == MINISURF_ABI16);
    legacy.dropRef(ref);
    args.structureOutputSize = 0x28;
    assert(legacy.methodInit(&args) == kIOReturnBadArgument);
    delete[] legacy.fRefs;
    assert(liveMaps == 0);
    puts("IOSurface mapping: failures, ABI negotiation/isolation and cleanup passed");
}
'''

ALLOC_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include <new>
#include "MiniIOSurfaceABI.h"

typedef int IOReturn;
typedef size_t vm_size_t;
typedef int task_t;
enum {
    kIOReturnSuccess = 0,
    kIOReturnUnsupported = -1,
    kIOReturnNoMemory = -2,
    kIOReturnBadArgument = -3,
    kIOReturnVMError = -4,
    kIOReturnNoResources = -5,
    kIOMapAnywhere = 1,
    kIOMapReadOnly = 2,
    kIODirectionInOut = 3,
    kIODirectionOut = 4
};
enum {
    PAGE_SIZE = 16384,
    PAGE_MASK = 0x3fff,
    MINISURF_ROW_ALIGN = 64,
    MINISURF_MAX_DIMENSION = 16384,
    MINISURF_TABLE_SIZE = 1024
};
#define MINISURF_MAX_ALLOC (512u << 20)
#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define IOLog(...) ((void)0)
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = NULL; } } while (0)
#define OSTypeAlloc(type) new (std::nothrow) type

static int gSurfaceLock = 0;
static void IOLockLock(int) {}
static void IOLockUnlock(int) {}

static uint32_t gSurfaceNext = 1;
static uint32_t gSurfaceCount = 0;
static uint64_t gSurfaceBytes = 0;
static unsigned gSurfaceUnheld = 0;

static task_t current_task(void) { return 1; }
static task_t kernel_task = 0;

struct OSObject {
    virtual ~OSObject() {}
    virtual void release() { delete this; }
    virtual void retain() {}
};
static OSObject *kOSBooleanTrue = (OSObject*)1;
static OSObject *kOSBooleanFalse = (OSObject*)2;

struct OSNumber : public OSObject {
    uint64_t val;
    OSNumber(uint64_t v) : val(v) {}
    uint32_t unsigned32BitValue() const { return (uint32_t)val; }
    uint64_t unsigned64BitValue() const { return val; }
    static OSNumber *withNumber(uint64_t v, unsigned) { return new OSNumber(v); }
};

struct OSDictionary : public OSObject {
    std::map<std::string, OSObject *> map;
    ~OSDictionary() {
        for (auto &kv : map) {
            if (kv.second && kv.second != kOSBooleanTrue && kv.second != kOSBooleanFalse) {
                kv.second->release();
            }
        }
    }
    OSObject *getObject(const char *key) {
        auto it = map.find(key);
        return it != map.end() ? it->second : NULL;
    }
    void setObject(const char *key, OSObject *val) {
        map[key] = val;
    }
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
};

struct OSArray : public OSObject {
    std::vector<OSObject *> items;
    ~OSArray() {
        for (auto item : items) {
            if (item && item != kOSBooleanTrue && item != kOSBooleanFalse) {
                item->release();
            }
        }
    }
    unsigned getCount() { return items.size(); }
    OSObject *getObject(unsigned i) { return items[i]; }
};

#define OSDynamicCast(type, obj) dynamic_cast<type*>(obj)

static size_t lastSurfaceMemoryAlloc = 0;
struct IOMemoryMap {
    uint8_t buffer[64];
    void *getBytesNoCopy() { return buffer; }
    void *getVirtualAddress() { return buffer; }
    void release() { delete this; }
};

struct Memory {
    uint8_t buffer[PAGE_SIZE * 2];
    IOMemoryMap map;
    void *getBytesNoCopy() { return buffer; }
    IOReturn prepare() { return kIOReturnSuccess; }
    IOMemoryMap *createMappingInTask(task_t, int, int) { return new IOMemoryMap; }
    void release() { delete this; }
};

static Memory *surfaceMemory(vm_size_t size) {
    lastSurfaceMemoryAlloc = size;
    return new Memory;
}

namespace IOMemoryDescriptor {
    inline Memory *withAddressRange(uint64_t, uint32_t, int, task_t) {
        return new Memory;
    }
}

struct minisurf_shared {
    uint64_t registryID;
    uint32_t seed;
    uint32_t valuesSeed;
    uint8_t  pixelSizeCasting;
    uint8_t  displayable;
    uint8_t  sysMemOnly;
};

struct MiniIOSurface : public OSObject {
    struct minisurf_result info;
    struct minisurf_plane planes[MINISURF_MAX_PLANES];
    Memory *memory;
    Memory *client;
    IOMemoryMap *clientMap;
    OSDictionary *values;
    uint8_t *kbase;
    uint8_t *kpixels;
    task_t clientTask;
    uint64_t clientAddress;
    uint32_t sid;
    uint32_t holders;
    struct minisurf_shared sharedState;

    bool init() {
        memset(&info, 0, sizeof(info));
        memset(planes, 0, sizeof(planes));
        memory = client = NULL;
        clientMap = NULL;
        values = NULL;
        kbase = kpixels = NULL;
        clientTask = 0;
        clientAddress = 0;
        sid = holders = 0;
        memset(&sharedState, 0, sizeof(sharedState));
        return true;
    }
    virtual ~MiniIOSurface() {
        if (memory) memory->release();
        if (client) client->release();
        if (clientMap) clientMap->release();
        if (values) values->release();
    }
    struct minisurf_shared *shared() { return &sharedState; }
};

static MiniIOSurface *gSurfaceTable[MINISURF_TABLE_SIZE] = {NULL};
'''

ALLOC_MAIN = r'''
int main(void)
{
    // Case 1: Non-wrapped surface, alloc > alignUp(needed, PAGE_SIZE)
    // Width 1206, Height 2622, format BGRA -> bpr = alignUp(1206*4, 64) = 4864
    // needed = 4864 * 2622 = 12753408
    // alignUp(12753408, 16384) = 12763136
    // Pass large alloc size (32MB). Kernel must clamp info.allocSize to alignUp(needed, PAGE_SIZE).
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(1206));
        props->setObject("IOSurfaceHeight", new OSNumber(2622));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('B','G','R','A')));
        props->setObject("IOSurfaceAllocSize", new OSNumber(33554432));
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 12763136);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE + 12763136);
        s->release();
        props->release();
    }

    // Case 2: Non-wrapped surface, alloc == alignUp(needed, PAGE_SIZE)
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(1206));
        props->setObject("IOSurfaceHeight", new OSNumber(2622));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('B','G','R','A')));
        props->setObject("IOSurfaceAllocSize", new OSNumber(12763136));
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 12763136);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE + 12763136);
        s->release();
        props->release();
    }

    // Case 3: Non-wrapped surface, alloc between needed and alignUp(needed, PAGE_SIZE)
    // needed = 12753408, alignUp = 12763136. Pass 12760000.
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(1206));
        props->setObject("IOSurfaceHeight", new OSNumber(2622));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('B','G','R','A')));
        props->setObject("IOSurfaceAllocSize", new OSNumber(12760000));
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 12760000);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE + 12763136);
        s->release();
        props->release();
    }

    // Case 4: Non-wrapped surface, alloc omitted (0) -> alloc defaults to needed
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(1206));
        props->setObject("IOSurfaceHeight", new OSNumber(2622));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('B','G','R','A')));
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 12753408);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE + 12763136);
        s->release();
        props->release();
    }

    // Case 5: Wrapped surface, alloc > alignUp(needed, PAGE_SIZE) -> NOT clamped!
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(1206));
        props->setObject("IOSurfaceHeight", new OSNumber(2622));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('B','G','R','A')));
        props->setObject("IOSurfaceAllocSize", new OSNumber(33554432));
        props->setObject("IOSurfaceAddress", new OSNumber(0x200000000ULL));
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 33554432);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE);
        s->release();
        props->release();
    }

    // Case 6: Multi-plane surface with explicit plane info, alloc > alignUp(needed, PAGE_SIZE)
    {
        OSDictionary *props = new OSDictionary();
        props->setObject("IOSurfaceWidth", new OSNumber(64));
        props->setObject("IOSurfaceHeight", new OSNumber(64));
        props->setObject("IOSurfacePixelFormat", new OSNumber(FOURCC('4','2','0','v')));
        props->setObject("IOSurfaceAllocSize", new OSNumber(65536));

        OSArray *planeArray = new OSArray();
        OSDictionary *p0 = new OSDictionary();
        p0->setObject("IOSurfacePlaneWidth", new OSNumber(64));
        p0->setObject("IOSurfacePlaneHeight", new OSNumber(64));
        p0->setObject("IOSurfacePlaneBytesPerRow", new OSNumber(64));
        p0->setObject("IOSurfacePlaneOffset", new OSNumber(0));
        p0->setObject("IOSurfacePlaneSize", new OSNumber(4096));
        planeArray->items.push_back(p0);

        OSDictionary *p1 = new OSDictionary();
        p1->setObject("IOSurfacePlaneWidth", new OSNumber(32));
        p1->setObject("IOSurfacePlaneHeight", new OSNumber(32));
        p1->setObject("IOSurfacePlaneBytesPerRow", new OSNumber(64));
        p1->setObject("IOSurfacePlaneOffset", new OSNumber(4096));
        p1->setObject("IOSurfacePlaneSize", new OSNumber(2048));
        planeArray->items.push_back(p1);

        props->setObject("IOSurfacePlaneInfo", planeArray);

        // needed = 4096 + 2048 = 6144. alignUp(6144, 16384) = 16384.
        IOReturn err = 0;
        MiniIOSurface *s = surfaceCreate(props, &err);
        assert(s != NULL && err == kIOReturnSuccess);
        assert(s->info.allocSize == 16384);
        assert(lastSurfaceMemoryAlloc == PAGE_SIZE + 16384);
        s->release();
        props->release();
    }

    puts("IOSurface allocation: non-wrapped clamping to alignUp(needed, PAGE_SIZE) and wrapped pass-through passed");
    return 0;
}
'''


def sources():
    driver = DRIVER.read_text()
    start = driver.index("struct minisurf_client_ref *\nIOSurfaceRootUserClient::addRef(")
    end = driver.index('#include "MiniIOSurfaceABI.inc"', start)
    methods = HEADER.with_suffix(".inc").read_text()
    init_start = methods.index("IOReturn\nIOSurfaceRootUserClient::methodInit(")
    init_end = methods.index("IOReturn\nIOSurfaceRootUserClient::methodSetBulkAttachments", init_start)
    alloc_start = driver.index("static uint32_t\ndictNumber(")
    alloc_end = driver.index("static void\nsurfaceHold(", alloc_start)
    return {
        "abi": ABI_SOURCE,
        "mapping": MAP_PREFIX + driver[start:end] + methods[init_start:init_end] + MAP_MAIN,
        "allocation": ALLOC_PREFIX + driver[alloc_start:alloc_end] + ALLOC_MAIN,
    }


def main():
    with tempfile.TemporaryDirectory() as directory:
        for name, body in sources().items():
            source = Path(directory) / (name + ".cc")
            executable = Path(directory) / name
            source.write_text("#include <initializer_list>\n" + body)
            subprocess.run([
                "clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-O1",
                "-fsanitize=address,undefined", "-I", str(HEADER.parent),
                str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

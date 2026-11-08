#!/usr/bin/env python3
"""Exercise production bridge methods with native CF serialization and host object adapters."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/files/iokit/Kernel/MiniIOSurface.cpp"
VALUES = SOURCE.with_name("MiniIOSurfaceValues.inc")

PREFIX = r'''
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFSerialize.h>
#include <IOKit/IOCFUnserialize.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "MiniIOSurfaceABI.h"
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=-1, kIOReturnUnsupported=-2;
static std::mutex lock;
static std::mutex *gSurfaceLock = &lock;
static thread_local bool locked;
static void IOLockLock(std::mutex *p) { assert(!locked); p->lock(); locked=true; }
static void IOLockUnlock(std::mutex *p) { assert(locked); locked=false; p->unlock(); }
static int copyout(const void *src, user_addr_t dst, size_t n) {
    assert(!locked);
    if (dst < 4096) return EFAULT;
    memcpy((void *)dst, src, n); return 0;
}
static std::atomic<int> live{0};
struct OSObject {
    OSObject() { ++live; }
    virtual ~OSObject() { --live; }
    void release() { delete this; }
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define OSSafeReleaseNULL(value) do { if(value) (value)->release(); (value)=nullptr; } while(0)
#define IODelete(p, type, count) delete[] (p)
struct OSString : OSObject {
    std::string text;
    explicit OSString(CFStringRef value) {
        std::vector<char> out(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),kCFStringEncodingUTF8)+1);
        assert(CFStringGetCString(value,out.data(),out.size(),kCFStringEncodingUTF8)); text=out.data();
    }
};
struct OSArray : OSObject {
    std::vector<OSObject *> values;
    explicit OSArray(CFArrayRef value) {
        for(CFIndex i=0;i<CFArrayGetCount(value);++i) {
            CFTypeRef item=CFArrayGetValueAtIndex(value,i);
            values.push_back(CFGetTypeID(item)==CFStringGetTypeID() ?
                (OSObject *)new OSString((CFStringRef)item) : new OSObject);
        }
    }
    ~OSArray() { for(auto p:values) p->release(); }
    unsigned getCount() { return values.size(); }
    OSObject *getObject(unsigned i) { return values.at(i); }
};
static OSObject *unserialize(const uint8_t *in, size_t size) {
    CFTypeRef value=IOCFUnserializeWithSize((const char *)in,size,kCFAllocatorDefault,0,nullptr);
    if(!value) return nullptr;
    OSObject *out=CFGetTypeID(value)==CFArrayGetTypeID() ? (OSObject *)new OSArray((CFArrayRef)value) : new OSObject;
    CFRelease(value); return out;
}
struct OSSerialize : OSObject {
    uint32_t size=0;
    static OSSerialize *binaryWithCapacity(unsigned) { return new OSSerialize; }
    uint32_t getLength() { return size; }
};
struct OSDictionary {
    std::set<std::string> keys;
    unsigned getCount() { assert(locked); return keys.size(); }
    OSDictionary *getObject(OSString *key) { assert(locked); return keys.count(key->text)?this:nullptr; }
    void removeObject(OSString *key) { assert(locked); keys.erase(key->text); }
    bool serialize(OSSerialize *s) { assert(locked); s->size=keys.size()*16; return true; }
};
struct minisurf_shared {
    uint8_t before[0x20]; uint32_t valuesSeed; uint32_t pad; uint32_t valuesSize; uint8_t after[0xc0-0x2c];
};
struct MiniIOSurface {
    minisurf_shared page={}; OSDictionary dictionary; OSDictionary *values=&dictionary;
    MiniIOSurface() { page.valuesSeed=1; }
    minisurf_shared *shared() { return &page; }
    void updateValuesState();
};
struct minisurf_client_ref { MiniIOSurface *surface; };
struct IOUserClient { void free() {} };
struct IOSurfaceRootUserClient : IOUserClient {
    minisurf_abi fABI=MINISURF_ABI26; OSArray *fCoreVideoBridgedKeys=nullptr;
    minisurf_client_ref *fRefs=nullptr, ref; unsigned fRefCount=0,fRefCapacity=0;
    explicit IOSurfaceRootUserClient(MiniIOSurface *surface) : ref{surface} {}
    minisurf_client_ref *findRef(uint32_t sid,bool) { assert(locked); return sid==17?&ref:nullptr; }
    void dropRef(minisurf_client_ref *) { assert(false); }
    void free();
    IOReturn methodSetCoreVideoBridgedKeys(const uint8_t *, size_t);
    IOReturn trapRemoveCoreVideoBridgedValues(void *,void *,void *,void *,void *,void *);
    IOReturn trapRemoveBulkAttachments(void *,void *,void *,void *,void *,void *);
};
'''

MAIN = r'''
static std::vector<uint8_t> wire(CFTypeRef object) {
    CFDataRef data=IOCFSerialize(object,1); assert(data);
    std::vector<uint8_t> out(12+CFDataGetLength(data),0xa5);
    memcpy(out.data()+12,CFDataGetBytePtr(data),CFDataGetLength(data)); CFRelease(data); return out;
}
static auto keys(std::initializer_list<CFTypeRef> list) -> std::vector<uint8_t> {
    std::vector<CFTypeRef> values(list); auto array=CFArrayCreate(nullptr,values.data(),values.size(),&kCFTypeArrayCallBacks);
    auto out=wire(array); CFRelease(array); return out;
}
static IOReturn set(IOSurfaceRootUserClient &c,const std::vector<uint8_t> &in) {
    return c.methodSetCoreVideoBridgedKeys(in.data(),in.size());
}
static IOReturn remove(IOSurfaceRootUserClient &c,uint32_t *seed,uint32_t sid=17) {
    return c.trapRemoveCoreVideoBridgedValues((void *)(uintptr_t)sid,seed,nullptr,nullptr,nullptr,nullptr);
}
static IOReturn bulk(IOSurfaceRootUserClient &c,uint64_t mask) {
    return c.trapRemoveBulkAttachments((void *)17,(void *)(uintptr_t)mask,nullptr,nullptr,nullptr,nullptr);
}
int main() {
    MiniIOSurface surface; auto &values=surface.dictionary.keys; auto &page=surface.page;
    IOSurfaceRootUserClient a(&surface),b(&surface);
    auto full=keys({CFSTR("IOSurfaceICCProfile"),CFSTR("IOSurfaceICCProfile"),kCFBooleanTrue});
    auto empty=keys({}); auto other=keys({CFSTR("IOSurfaceColorSpace")});
    uint32_t seed=0xdeadbeef;
    assert(remove(a,&seed)==kIOReturnBadArgument && seed==0xdeadbeef);
    assert(set(a,full)==0);
    assert(set(b,other)==0);
    values={"IOSurfaceICCProfile","SyntheticKeep","CVImageBufferColorPrimaries"};
    memset(minisurfAttachments(&page,MINISURF_ABI26),0x7e,0x84);
    assert(remove(b,&seed)==0 && seed==0 && page.valuesSeed==1 && values.size()==3);
    assert(remove(a,&seed)==0 && seed==3 && page.valuesSeed==3 && values.size()==2);
    assert(values.count("SyntheticKeep") && values.count("CVImageBufferColorPrimaries"));
    for(unsigned i=0;i<0x84;++i) if(i!=0x67) assert(minisurfAttachments(&page,MINISURF_ABI26)[i]==0);
    assert(remove(a,&seed)==0 && seed==0 && page.valuesSeed==3);
    assert(set(a,empty)==0); values.insert("IOSurfaceICCProfile");
    assert(remove(a,&seed)==0 && seed==0 && values.size()==3);
    assert(set(a,full)==0);
    auto wrong=wire(CFSTR("not an array"));
    assert(set(a,wrong)==kIOReturnBadArgument);
    assert(set(a,std::vector<uint8_t>(16,0))==kIOReturnBadArgument);
    uint8_t shortInput[15]={};
    assert(a.methodSetCoreVideoBridgedKeys(shortInput,sizeof(shortInput))==kIOReturnBadArgument);
    assert(remove(a,&seed)==0 && seed==5 && values.size()==2);
    seed=0xdeadbeef; assert(remove(a,&seed,0)==kIOReturnBadArgument && seed==0xdeadbeef);
    assert(remove(a,nullptr)==EFAULT); assert(remove(a,(uint32_t *)1)==EFAULT);
    auto bytes=minisurfAttachments(&page,MINISURF_ABI26); bytes[0x3b]=9; bytes[0x3c]=7;
    assert(bulk(a,1ULL<<6)==0 && bytes[0x3b]==0 && bytes[0x3c]==7 && page.valuesSeed==5);
    assert(bulk(a,(1ULL<<63)|(1ULL<<7))==kIOReturnBadArgument && bytes[0x3c]==7);
    assert(bulk(a,0)==0 && bytes[0x3c]==7);
    assert(bulk(a,MINISURF_BULK_MASK26)==0 && bytes[0x3c]==0 && page.valuesSeed==5);
    b.fABI=MINISURF_ABI16;
    assert(set(b,full)==kIOReturnUnsupported && remove(b,&seed)==kIOReturnUnsupported);
    assert(bulk(b,1)==kIOReturnUnsupported);
    std::thread setter([&] { for(int i=0;i<1000;++i) assert(set(a,i%2?full:empty)==0); });
    std::thread remover([&] { uint32_t out; for(int i=0;i<1000;++i) assert(remove(a,&out)==0); });
    setter.join(); remover.join();
    a.free(); b.free(); assert(live==0);
    puts("IOSurface bridge: native CF wire, per-client replacement, removal, seeds, masks, errors, concurrency and release passed");
}
'''


def source_text():
    driver = SOURCE.read_text()
    start = driver.index("void\nMiniIOSurface::updateValuesState(")
    end = driver.index("\nstatic uint32_t", start)
    free_start = driver.index("void\nIOSurfaceRootUserClient::free(")
    free_end = driver.index("/* All three with the lock held. */", free_start)
    bridge = VALUES.read_text()
    bridge = bridge[bridge.index("/* The 12-byte prefix"):]
    return PREFIX + driver[start:end] + driver[free_start:free_end] + bridge + MAIN


def main():
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "bridge.cc"
        executable = Path(directory) / "bridge"
        source.write_text(source_text())
        subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-O1", "-fsanitize=address,undefined",
                        "-I", str(SOURCE.parent), str(source), "-framework", "CoreFoundation",
                        "-framework", "IOKit", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

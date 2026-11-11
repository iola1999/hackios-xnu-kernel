#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct OSObject {
    virtual ~OSObject() = default;
    void retain() {}
    void release() {}
};
static std::vector<std::unique_ptr<OSObject>> arena;
template <class T, class... A> static T *make(A &&...args)
{
    auto object = std::make_unique<T>(std::forward<A>(args)...);
    T *pointer = object.get();
    arena.push_back(std::move(object));
    return pointer;
}
struct OSNumber : OSObject {
    uint64_t number;
    explicit OSNumber(uint64_t value) : number(value) {}
    uint64_t unsigned64BitValue() const { return number; }
    static OSNumber *withNumber(uint64_t value, unsigned) { return make<OSNumber>(value); }
};
struct OSSymbol : OSObject {
    std::string text;
    explicit OSSymbol(std::string value) : text(std::move(value)) {}
    const char *getCStringNoCopy() const { return text.c_str(); }
};
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> values;
    static OSDictionary *withCapacity(unsigned) { return make<OSDictionary>(); }
    OSObject *getObject(const char *key) const
    {
        auto found = values.find(key);
        return found == values.end() ? nullptr : found->second;
    }
    OSObject *getObject(const OSSymbol *key) const { return getObject(key->getCStringNoCopy()); }
    void setObject(const char *key, OSObject *value) { values[key] = value; }
};
struct OSCollectionIterator : OSObject {
    std::vector<OSSymbol *> keys;
    size_t next = 0;
    explicit OSCollectionIterator(OSDictionary *dict)
    {
        for (const auto &item : dict->values) keys.push_back(make<OSSymbol>(item.first));
    }
    static OSCollectionIterator *withCollection(OSDictionary *dict) { return make<OSCollectionIterator>(dict); }
    OSObject *getNextObject() { return next < keys.size() ? keys[next++] : nullptr; }
};
struct OSBoolean : OSObject {};
static OSBoolean host_true;
static OSBoolean *kOSBooleanTrue = &host_true;
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define OSSafeReleaseNULL(value) do { if (value != nullptr) value->release(); value = nullptr; } while (0)

struct IOService : OSObject {
    std::map<std::string, OSObject *> properties;
    void setProperty(const char *key, OSObject *value) { properties[key] = value; }
};
using IOReturn = int;
static constexpr IOReturn kIOReturnSuccess = 0;
static constexpr IOReturn kIOReturnBadArgument = -1;
static constexpr IOReturn kIOReturnUnsupported = -2;
class AppleARMBacklight : public IOService {
public:
    IOReturn setProperties(OSObject *properties);
    void publish(uint64_t value, uint64_t millinits);
};
struct IOLock {};
using thread_call_t = void *;
static int lock_depth;
static void IOLockLock(IOLock *) { ++lock_depth; }
static void IOLockUnlock(IOLock *) { --lock_depth; }
static uint64_t host_now, host_touch;
static constexpr uint64_t NSEC_PER_MSEC = 1000000;
static uint64_t mach_absolute_time() { return host_now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *out) { *out = ns; }
static uint64_t annibale_cpufreq_touched() { return host_touch; }
static std::vector<uint64_t> delays;
static void thread_call_enter_delayed(thread_call_t, uint64_t deadline) { delays.push_back(deadline); }
static std::vector<std::string> logs;
static void IOLog(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    logs.emplace_back(line);
}
static void proc_selfname(char *name, size_t bytes) { snprintf(name, bytes, "host"); }
static int proc_selfpid() { return 7; }
static bool use_dcs;
static std::vector<uint32_t> dcs_values, dim_values;
static bool MiniIOMobileFramebufferDCS(uint32_t value)
{
    dcs_values.push_back(value);
    return use_dcs;
}
static void MiniIOMobileFramebufferDim(uint32_t value) { dim_values.push_back(value); }
static int MiniIOMobileFramebufferDimShown() { return -1; }
static std::vector<std::pair<uint32_t, uint32_t>> saves;
static void ramfs_backlight_save(uint32_t value, uint32_t millinits) { saves.emplace_back(value, millinits); }
static uint32_t annibale_backlight_level;
static void setNumber(OSDictionary *dict, const char *key, uint64_t value)
{
    dict->setObject(key, OSNumber::withNumber(value, 64));
}

// This include contains the actual functions extracted from the candidate or baseline source.
#include "compiled-backlight.inc"

static AppleARMBacklight service;
static IOLock host_lock;
static OSDictionary *dict() { return OSDictionary::withCapacity(4); }
static void number(OSDictionary *dict, const char *key, uint64_t value) { setNumber(dict, key, value); }
static OSDictionary *nested(const char *key, uint64_t value, bool physical = true)
{
    auto request = dict();
    auto brightness = dict();
    number(brightness, key, value);
    number(brightness, "Brightness", 0);
    if (physical && strcmp(key, "NitsPhysical") == 0) number(brightness, "Nits", 60);
    request->setObject("DisplayBrightness", brightness);
    return request;
}
static OSDictionary *fixed(const char *key, uint64_t value)
{
    auto request = dict();
    number(request, key, value);
    return request;
}
static void check(bool passed, const char *message)
{
    if (!passed) throw std::runtime_error(message);
}
static uint64_t getNumber(OSDictionary *dict, const char *key)
{
    auto number = OSDynamicCast(OSNumber, dict->getObject(key));
    check(number != nullptr, "missing published number");
    return number->unsigned64BitValue();
}
static void reset()
{
    service.properties.clear();
    arena.clear();
    bl = {};
    bl.lock = &host_lock;
    bl.service = &service;
    bl.last_v = kBLValueMax;
    bl.last_millinits = kBLMilliNitsMax;
    bl.initialized = true;
    host_now = 10000000000;
    host_touch = 0;
    lock_depth = 0;
    annibale_backlight_level = kBLLevelFull;
    use_dcs = true;
    delays.clear();
    logs.clear();
    dcs_values.clear();
    dim_values.clear();
    saves.clear();
}
static void backlight(OSObject *request)
{
    check(service.setProperties(request) == kIOReturnSuccess, "backlight request failed");
    check(lock_depth == 0, "backlight lock retained");
}
static void framebuffer(OSObject *request)
{
    check(MiniAppleARMBacklightFramebufferProperties(request) == kIOReturnSuccess, "framebuffer request failed");
    check(lock_depth == 0, "framebuffer lock retained");
}
static void applied(uint64_t millinits, uint64_t value)
{
    check(bl.last_millinits == millinits, "wrong nits conversion");
    check(bl.last_v == value, "wrong panel conversion");
    check(!dcs_values.empty() && dcs_values.back() == value, "wrong DCS request");
    auto parameters = OSDynamicCast(OSDictionary, service.properties["IODisplayParameters"]);
    check(parameters != nullptr, "IODisplayParameters missing");
    auto brightness = OSDynamicCast(OSDictionary, parameters->getObject("brightness"));
    auto nits = OSDynamicCast(OSDictionary, parameters->getObject("BrightnessMilliNits"));
    check(brightness != nullptr && nits != nullptr, "published ranges missing");
    check(getNumber(brightness, "min") == 5 && getNumber(brightness, "max") == 6750, "panel range changed");
    check(getNumber(nits, "min") == 5000 && getNumber(nits, "max") == 400000, "nits range changed");
    check(getNumber(brightness, "value") == value && getNumber(nits, "value") == millinits, "published state mismatch");
}
using Case = std::pair<std::string, std::function<void()>>;
static std::vector<Case> cases()
{
    return {
        {"marketing_midpoint_supplies_140_nits", [] {
            check(sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0]) == 11, "marketing table size changed");
            check(kBLMarketingMilliNits[5] == 140000 && fixed_nits(kBLMarketingMilliNits[5]) == 9175040, "marketing midpoint changed");
        }},
        {"physical_120", [] { backlight(nested("NitsPhysical", 120)); applied(120000, 1968); }},
        {"physical_113", [] { backlight(nested("NitsPhysical", 113)); applied(113000, 1849); }},
        {"physical_109", [] { backlight(nested("NitsPhysical", 109)); applied(109000, 1780); }},
        {"nits_fallback_60", [] { backlight(nested("Nits", 60)); applied(60000, 944); }},
        {"physical_precedes_nits", [] { backlight(nested("NitsPhysical", 120)); applied(120000, 1968); }},
        {"plain_one_clamps_to_min", [] { backlight(nested("NitsPhysical", 1)); applied(5000, 5); }},
        {"plain_min", [] { backlight(nested("NitsPhysical", 5)); applied(5000, 5); }},
        {"plain_max", [] { backlight(nested("NitsPhysical", 400)); applied(400000, 6750); }},
        {"plain_above_max", [] { backlight(nested("NitsPhysical", 1000)); applied(400000, 6750); }},
        {"plain_large_no_overflow", [] { backlight(nested("NitsPhysical", INT64_MAX)); applied(400000, 6750); }},
        {"plain_multiply_overflow_boundary", [] { backlight(nested("NitsPhysical", 18446744073709552ULL)); applied(400000, 6750); }},
        {"fixed_120_backlight", [] { backlight(fixed("brightness-nits", 7864320)); applied(120000, 1968); }},
        {"fixed_120_framebuffer", [] { framebuffer(fixed("IOMFBBrightnessLevel", 7864320)); applied(120000, 1968); }},
        {"fixed_scene_117", [] { framebuffer(fixed("IOMFBBrightnessLevel", 7680368)); applied(117193, 1920); }},
        {"fixed_fractional_clamps", [] { framebuffer(fixed("IOMFBBrightnessLevel", 32768)); applied(5000, 5); }},
        {"fixed_one_raw_clamps", [] { backlight(fixed("brightness-nits", 1)); applied(5000, 5); }},
        {"fixed_mid_no_brightness_guess", [] {
            auto request = nested("NitsPhysical", 1);
            number(request, "brightness-nits", 9175040);
            backlight(request);
            applied(140000, 2310);
        }},
        {"fixed_large_no_overflow", [] { framebuffer(fixed("IOMFBBrightnessLevel", INT64_MAX)); applied(400000, 6750); }},
        {"fixed_multiply_overflow_boundary", [] { framebuffer(fixed("IOMFBBrightnessLevel", 18446744073709552ULL)); applied(400000, 6750); }},
        {"nvram_is_fixed", [] { backlight(fixed("DisplayBrightnessNitsNVRAM", 7864320)); applied(120000, 1968); }},
        {"dictionary_precedes_nvram", [] {
            auto request = nested("NitsPhysical", 120);
            number(request, "DisplayBrightnessNitsNVRAM", 13107200);
            backlight(request);
            applied(120000, 1968);
        }},
        {"top_fixed_precedes_dictionary", [] {
            auto request = nested("NitsPhysical", 120);
            number(request, "brightness-nits", 13107200);
            backlight(request);
            applied(200000, 3334);
        }},
        {"panel_precedes_dictionary", [] {
            auto request = nested("NitsPhysical", 120);
            number(request, "brightness", 6750);
            backlight(request);
            applied(400000, 6750);
        }},
        {"top_fixed_precedes_panel", [] {
            auto request = fixed("brightness", 6750);
            number(request, "brightness-nits", 7864320);
            backlight(request);
            applied(120000, 1968);
        }},
        {"plain_and_fixed_same_value", [] {
            backlight(nested("NitsPhysical", 120));
            uint64_t prior = bl.last_v;
            framebuffer(fixed("IOMFBBrightnessLevel", 7864320));
            applied(120000, prior);
            check(saves.empty(), "idle request persisted");
        }},
        {"physical_zero_preserves_saved", [] {
            host_touch = host_now;
            auto request = nested("NitsPhysical", 0);
            backlight(request);
            check(bl.last_millinits == 400000 && bl.last_v == 6750, "off changed saved state");
            check(saves.empty() && dcs_values.back() == 0, "off persisted or did not reach DCS");
        }},
        {"fixed_zero_preserves_saved", [] {
            host_touch = host_now;
            framebuffer(fixed("IOMFBBrightnessLevel", 0));
            check(bl.last_millinits == 400000 && saves.empty(), "fixed off changed saved state");
            check(dcs_values.back() == 0, "fixed off did not reach DCS");
        }},
        {"fixed_negative_is_off", [] {
            framebuffer(fixed("IOMFBBrightnessLevel", UINT64_MAX));
            check(bl.last_millinits == 400000 && dcs_values.back() == 0, "negative nits did not turn off");
        }},
        {"panel_zero_is_off", [] {
            backlight(fixed("brightness", 0));
            check(bl.last_v == 6750 && dcs_values.back() == 0 && saves.empty(), "panel off changed state");
        }},
        {"panel_range_min", [] { backlight(fixed("brightness", 1)); applied(5000, 5); }},
        {"panel_range_max", [] { backlight(fixed("brightness", 8000)); applied(400000, 6750); }},
        {"recent_touch_persists", [] {
            host_touch = host_now - 3000000000;
            backlight(nested("NitsPhysical", 120));
            applied(120000, 1968);
            check(saves.size() == 1 && saves.back() == std::make_pair(1968u, 120000u), "recent touch not persisted");
            check(service.properties["annibale-bl-restored"] == kOSBooleanTrue, "restore state missing");
        }},
        {"expired_touch_does_not_persist", [] {
            host_touch = host_now - 3000000001;
            backlight(nested("NitsPhysical", 120));
            applied(120000, 1968);
            check(saves.empty(), "expired touch persisted");
        }},
        {"future_touch_does_not_persist", [] {
            host_touch = host_now + 1;
            backlight(nested("NitsPhysical", 120));
            check(saves.empty(), "future touch persisted");
        }},
        {"slider_first_records_position", [] {
            host_touch = host_now;
            auto request = nested("NitsPhysical", 120);
            number(request, "DisplayBrightnessSliderPosition", 32768);
            backlight(request);
            applied(120000, 1968);
            check(bl.slider_initialized && bl.last_slider_pos == 32768 && bl.user_until == 0, "first slider changed its contract");
        }},
        {"slider_drag_uses_native_nits", [] {
            host_touch = host_now;
            auto initial = nested("NitsPhysical", 5);
            number(initial, "DisplayBrightnessSliderPosition", 0);
            backlight(initial);
            applied(5000, 5);
            auto moved = nested("NitsPhysical", 140);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            applied(140000, 2310);
            check(bl.user_until == host_now + 400000000 && saves.size() == 2, "slider window or persistence changed");
            check(saves.back() == std::make_pair(2310u, 140000u), "slider saved a different brightness");
        }},
        {"slider_and_iomfb_do_not_alternate", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto moved = nested("NitsPhysical", 140);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            applied(140000, 2310);
            framebuffer(fixed("IOMFBBrightnessLevel", 9175040));
            applied(140000, 2310);
            backlight(moved);
            applied(140000, 2310);
            framebuffer(fixed("IOMFBBrightnessLevel", 9175040));
            applied(140000, 2310);
            check(dcs_values == std::vector<uint32_t>({2310, 2310, 2310, 2310}), "same nits alternated panel values");
            check(saves.size() == 4 && saves.back() == std::make_pair(2310u, 140000u), "same nits did not persist");
        }},
        {"slider_top_fixed_uses_native_nits", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto moved = fixed("brightness-nits", 9175040);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            applied(140000, 2310);
            framebuffer(fixed("IOMFBBrightnessLevel", 9175040));
            check(dcs_values == std::vector<uint32_t>({2310, 2310}), "fixed nits changed after slider request");
        }},
        {"nested_slider_uses_native_nits", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto moved = nested("NitsPhysical", 140);
            auto brightness = OSDynamicCast(OSDictionary, moved->getObject("DisplayBrightness"));
            number(brightness, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            applied(140000, 2310);
            check(bl.last_slider_pos == 32768 && bl.user_until == host_now + 400000000, "nested slider did not record interaction");
        }},
        {"slider_only_records_window", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            backlight(fixed("DisplayBrightnessSliderPosition", 32768));
            check(bl.last_slider_pos == 32768 && bl.user_until == host_now + 400000000, "slider-only request lost interaction");
            check(bl.last_millinits == 400000 && bl.last_v == 6750, "slider-only request changed brightness");
            check(dcs_values.empty() && saves.empty(), "slider-only request reached panel or persistence");
        }},
        {"slider_window_persists_after_touch_expires", [] {
            host_touch = host_now - 3000000000;
            bl.slider_initialized = true;
            auto moved = nested("NitsPhysical", 140);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            host_now += 20000000;
            framebuffer(fixed("IOMFBBrightnessLevel", 9175040));
            applied(140000, 2310);
            check(saves.size() == 2 && saves.back() == std::make_pair(2310u, 140000u), "slider window did not outlast touch window");
        }},
        {"slider_off_preserves_saved", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto moved = nested("NitsPhysical", 0);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            check(bl.last_millinits == 400000 && bl.last_v == 6750, "slider off changed saved state");
            check(saves.empty() && dcs_values == std::vector<uint32_t>({0}), "slider off persisted or missed DCS");
        }},
        {"slider_panel_value_keeps_panel_contract", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto moved = fixed("brightness", 6750);
            number(moved, "DisplayBrightnessSliderPosition", 32768);
            backlight(moved);
            applied(400000, 6750);
        }},
        {"slider_threshold_is_inclusive", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            bl.last_slider_pos = 1000;
            auto moved = nested("NitsPhysical", 140);
            number(moved, "DisplayBrightnessSliderPosition", 1064);
            backlight(moved);
            applied(140000, 2310);
            check(bl.last_slider_pos == 1064 && bl.user_until == host_now + 400000000, "64-step slider threshold changed");
        }},
        {"slider_position_clamps_without_applying", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            backlight(fixed("DisplayBrightnessSliderPosition", 100000));
            check(bl.last_slider_pos == 65536 && bl.user_until == host_now + 400000000, "slider position did not clamp");
            check(dcs_values.empty() && saves.empty(), "clamped slider-only request reached panel or persistence");
        }},
        {"slider_small_difference_uses_nits", [] {
            host_touch = host_now;
            auto initial = nested("NitsPhysical", 120);
            number(initial, "DisplayBrightnessSliderPosition", 1000);
            backlight(initial);
            auto moved = nested("NitsPhysical", 113);
            number(moved, "DisplayBrightnessSliderPosition", 1063);
            backlight(moved);
            applied(113000, 1849);
            check(bl.last_slider_pos == 1000 && bl.user_until == 0, "small slider movement changed threshold");
        }},
        {"idle_slider_uses_nits", [] {
            auto initial = nested("NitsPhysical", 120);
            number(initial, "DisplayBrightnessSliderPosition", 0);
            backlight(initial);
            auto moved = nested("NitsPhysical", 113);
            number(moved, "DisplayBrightnessSliderPosition", 65536);
            backlight(moved);
            applied(113000, 1849);
            check(saves.empty() && bl.last_slider_pos == 65536, "idle slider persisted");
        }},
        {"active_window_persists_framebuffer", [] {
            bl.user_until = host_now + 1;
            framebuffer(fixed("IOMFBBrightnessLevel", 7864320));
            check(saves.size() == 1, "active slider window did not persist");
        }},
        {"expired_window_does_not_persist", [] {
            bl.user_until = host_now - 1;
            framebuffer(fixed("IOMFBBrightnessLevel", 7864320));
            check(saves.empty(), "expired slider window persisted");
        }},
        {"framebuffer_ignores_dictionary_slider", [] {
            host_touch = host_now;
            bl.slider_initialized = true;
            auto request = nested("NitsPhysical", 1);
            number(request, "DisplayBrightnessSliderPosition", 65536);
            number(request, "IOMFBBrightnessLevel", 7864320);
            framebuffer(request);
            applied(120000, 1968);
            check(bl.last_slider_pos == 0, "framebuffer read slider position");
        }},
        {"no_dcs_keeps_dim_mapping", [] {
            use_dcs = false;
            backlight(nested("NitsPhysical", 120));
            applied(120000, 1968);
            check(annibale_backlight_level == 307 && dim_values.back() == 307, "dim conversion changed");
        }},
        {"dcs_keeps_full_mixer", [] {
            annibale_backlight_level = 512;
            backlight(nested("NitsPhysical", 120));
            check(annibale_backlight_level == 1024 && dim_values.back() == 1024, "DCS did not keep mixer full");
            backlight(nested("NitsPhysical", 120));
            check(dim_values.size() == 1, "unchanged mixer was rewritten");
        }},
        {"unknown_request_keeps_state", [] {
            auto request = fixed("Brightness", 120);
            backlight(request);
            check(bl.last_v == 6750 && dcs_values.empty() && saves.empty(), "unknown key changed state");
        }},
        {"framebuffer_requires_own_key", [] {
            check(MiniAppleARMBacklightFramebufferProperties(nested("NitsPhysical", 120)) == kIOReturnUnsupported, "framebuffer accepted wrong key");
            check(dcs_values.empty(), "refused framebuffer request reached DCS");
        }},
        {"bad_object_rejected", [] {
            auto object = make<OSNumber>(120);
            check(service.setProperties(object) == kIOReturnBadArgument, "backlight accepted non-dictionary");
            check(MiniAppleARMBacklightFramebufferProperties(object) == kIOReturnUnsupported, "framebuffer accepted non-dictionary");
            check(dcs_values.empty(), "invalid request reached DCS");
        }}
    };
}
int main(int argc, char **argv)
{
    unsigned passed = 0, failed = 0;
    for (auto &item : cases()) {
        if (argc == 2 && item.first != argv[1]) continue;
        reset();
        try {
            item.second();
            ++passed;
            printf("PASS %s\n", item.first.c_str());
        } catch (const std::exception &error) {
            ++failed;
            printf("FAIL %s: %s\n", item.first.c_str(), error.what());
        }
    }
    printf("RESULT %u passed %u failed\n", passed, failed);
    return failed != 0 || passed == 0;
}

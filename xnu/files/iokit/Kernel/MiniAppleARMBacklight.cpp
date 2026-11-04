/*
 * MiniAppleARMBacklight: a stand-in for Apple's AppleARMBacklight, the service CoreBrightness sets the
 * built-in display's brightness through (the brightness slider of Control Center and of Settings).
 *
 * backboardd's CoreBrightness takes the service that matches {IOPropertyMatch: {backlight-control:
 * true}} and conforms to AppleARMBacklight, reads its IODisplayParameters -- brightness and
 * BrightnessMilliNits, each {min, max, value} -- and from then on sets either brightness (a value in
 * that range, with FadePeriod) or brightness-nits (nits in 16.16 fixed point) on it. It also sets
 * IOMFBBrightnessLevel (nits, 16.16) on the IOMobileFramebuffer, which hands it on to here. Where in
 * iOS 26's shared cache each of these is: docs/analysis/2026-11-02-brightness-control.md §3.
 *
 * The panel is told nothing and stays as bright as the loader left it (DCS 0x51 would change that).
 * Every value becomes redwood_backlight_level, the fraction of that brightness to show, and with
 * boot-arg fb_dim=1 the display controller darkens the picture by it (MiniFramebufferDim.inc); without
 * it the service answers the same and nothing gets darker. Every request is logged, with all its keys:
 * one line in kBLLogMS at most, and the last of a burst always.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

/*
 * brightness: the panel's own range, from its minimum (qcom,mdss-dsi-bl-min-level) to its factory
 * ceiling (mi,mdss-fac-brightness-max-level; what is above it is taken to be high brightness, inferred).
 * BrightnessMilliNits: what CoreBrightness assumes of a display without calibration, 5 to 400 nits
 * (inferred). The two are mapped onto each other linearly. Both start at their maximum, as the picture
 * does: not darkened.
 */
#define kBLValueMin             5u
#define kBLValueMax             0x1a5eu
#define kBLMilliNitsMin         5000u
#define kBLMilliNitsMax         400000u
#define kBLLevelFull            1024u
#define kBLLogMS                100u

/*
 * What CoreBrightness's display setup (23G90, the function at 0x1e2741c1c) reads besides IODisplayParameters,
 * through -[CBBacklightNode ...]: its CBIORegistryReader looks in this service and then in its parents in the
 * IOService plane (0x1e26bd4b4: plane "IOService", options 3, kIORegistryIterateRecursively | Parents).
 *
 * backlight-calibration-parameters (getBacklightCalibration:, 0x1e2731760): numbers in 16.16. Without them the
 * setup works its luminances out from the marketing table before it has loaded one, and reads address 0
 * (0x1e2742294 -> 0x1e2746924): backboardd crashed and was started again, in a loop. Panel and product get the
 * same minimum, middle and maximum, the ones CoreBrightness assumes without a calibration (5, 140 and 400
 * nits, 0x1e2742248), so that its mapping from the one to the other changes nothing. The three currents are
 * only logged there (0x1e2742388 to 0x1e2742560) and are not known for this panel: 0.
 *
 * backlight-marketing-table (copyMarketingTable, 0x1e2731ad8): data of 32-bit 16.16 values (loadIOFixedArray:),
 * the nits at evenly spaced points from 0 to 1, interpolated between neighbours (0x1e2746810). Without it the
 * display is not set up at all ("Failed to load marketing table", 0x1e2742d14). The curve is a guess,
 * 5 + 395 x^1.55: through the same 5, 140 and 400 nits at 0, 0.5 and 1.
 */
#define kBLMilliNitsMid         140000u
static const uint32_t kBLMarketingMilliNits[] = {
	5000, 16160, 37660, 66190, 100550, 140000, 184050, 232340, 284570, 340520, 400000,
};

static inline uint64_t
fixed_nits(uint64_t millinits)
{
	return (millinits << 16) / 1000;
}

/* What was last asked for: the fraction of the panel's brightness to show, in 1024ths (nits over kBLMilliNitsMax). */
extern "C" {
uint32_t redwood_backlight_level = kBLLevelFull;
}

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void publish(uint64_t value, uint64_t millinits);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static struct {
	AppleARMBacklight *     service;
	IOLock *                lock;           /* around this, the level, and the mixers' writes */
	thread_call_t           later;          /* prints the last request of a burst */
	uint64_t                logged;         /* when the last line was printed */
	uint32_t                unlogged;       /* requests since then, not printed yet */
	char                    line[320];      /* the last request, as it is printed */
} bl;

static OSDictionary *
range(uint64_t min, uint64_t max, uint64_t value)
{
	OSDictionary * dict = OSDictionary::withCapacity(3);

	if (dict != NULL) {
		setNumber(dict, "min", min);
		setNumber(dict, "max", max);
		setNumber(dict, "value", value);
	}
	return dict;
}

/* IODisplayParameters, whole and new each time: user space may be reading the one before. */
void
AppleARMBacklight::publish(uint64_t value, uint64_t millinits)
{
	OSDictionary * parameters = OSDictionary::withCapacity(2);
	OSDictionary * brightness = range(kBLValueMin, kBLValueMax, value);
	OSDictionary * nits = range(kBLMilliNitsMin, kBLMilliNitsMax, millinits);

	if (parameters != NULL && brightness != NULL && nits != NULL) {
		parameters->setObject("brightness", brightness);
		parameters->setObject("BrightnessMilliNits", nits);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(nits);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
}

/* backlight-calibration-parameters and backlight-marketing-table, once: they do not change. */
static void
publish_calibration(IOService * service)
{
	OSDictionary * calibration = OSDictionary::withCapacity(9);
	uint32_t table[sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0])];
	OSData * marketing;

	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		table[i] = (uint32_t) fixed_nits(kBLMarketingMilliNits[i]);
	}
	marketing = OSData::withBytes(table, sizeof(table));
	if (calibration != NULL) {
		setNumber(calibration, "lMinPanel", fixed_nits(kBLMilliNitsMin));
		setNumber(calibration, "lMidPanel", fixed_nits(kBLMilliNitsMid));
		setNumber(calibration, "lMaxPanel", fixed_nits(kBLMilliNitsMax));
		setNumber(calibration, "lMinProduct", fixed_nits(kBLMilliNitsMin));
		setNumber(calibration, "lMidProduct", fixed_nits(kBLMilliNitsMid));
		setNumber(calibration, "lMaxProduct", fixed_nits(kBLMilliNitsMax));
		setNumber(calibration, "current-for-max-backlight", 0);
		setNumber(calibration, "current-for-mid-backlight", 0);
		setNumber(calibration, "hardware-max-current-limit", 0);
		service->setProperty("backlight-calibration-parameters", calibration);
	}
	if (marketing != NULL) {
		service->setProperty("backlight-marketing-table", marketing);
	}
	OSSafeReleaseNULL(marketing);
	OSSafeReleaseNULL(calibration);
}

/* " key=value" for every key of DICT (a number's value, '?' for anything else), after the USED bytes of LINE. */
static size_t
list_keys(char * line, size_t size, size_t used, OSDictionary * dict)
{
	OSCollectionIterator * each = OSCollectionIterator::withCollection(dict);
	OSSymbol * key;

	while (each != NULL && used < size && (key = OSDynamicCast(OSSymbol, each->getNextObject())) != NULL) {
		OSNumber * n = OSDynamicCast(OSNumber, dict->getObject(key));

		if (n != NULL) {
			used += (size_t) snprintf(line + used, size - used, " %s=%llu", key->getCStringNoCopy(), n->unsigned64BitValue());
		} else {
			used += (size_t) snprintf(line + used, size - used, " %s=?", key->getCStringNoCopy());
		}
	}
	OSSafeReleaseNULL(each);
	return used < size ? used : size;
}

static void
bl_print(uint64_t now)
{
	if (bl.unlogged > 1) {
		IOLog("MiniAppleARMBacklight: %s (the last of %u since the line before)\n", bl.line, bl.unlogged);
	} else {
		IOLog("MiniAppleARMBacklight: %s\n", bl.line);
	}
	bl.logged = now;
	bl.unlogged = 0;
}

/* With bl.lock held: bl.line printed now, or once kBLLogMS have passed since the line before. */
static void
bl_log(void)
{
	uint64_t now = mach_absolute_time(), interval;

	nanoseconds_to_absolutetime(kBLLogMS * NSEC_PER_MSEC, &interval);
	bl.unlogged++;
	if (bl.logged == 0 || now - bl.logged >= interval) {
		bl_print(now);
	} else if (bl.unlogged == 1) {
		thread_call_enter_delayed(bl.later, bl.logged + interval);
	}
}

static void
bl_log_later(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(bl.lock);
	if (bl.unlogged != 0) {
		bl_print(mach_absolute_time());
	}
	IOLockUnlock(bl.lock);
}

/*
 * A request to either service, with bl.lock held. NITS_KEY (16.16) is taken if it is there, else
 * VALUE_KEY (a value of the brightness range, none for the framebuffer); either way the result is a
 * value, nits, and the level, which the mixers are given when it changes.
 */
static void
bl_take(OSDictionary * dict, const char * value_key, const char * nits_key)
{
	OSNumber * nits = OSDynamicCast(OSNumber, dict->getObject(nits_key));
	OSNumber * value = value_key != NULL ? OSDynamicCast(OSNumber, dict->getObject(value_key)) : NULL;
	uint64_t v, millinits;
	uint32_t level;
	int alpha;
	char who[24] = "?", shown[32];
	size_t used;

	proc_selfname(who, sizeof(who));
	used = (size_t) snprintf(bl.line, sizeof(bl.line), "%s[%d] to the %s:", who, proc_selfpid(),
	    value_key != NULL ? "backlight" : "framebuffer");
	used = list_keys(bl.line, sizeof(bl.line), used < sizeof(bl.line) ? used : sizeof(bl.line), dict);
	if (nits != NULL) {
		uint64_t n16 = nits->unsigned64BitValue();

		/* (the bound only keeps the multiplication in 64 bits) */
		millinits = (n16 < (uint64_t) kBLMilliNitsMax << 16 ? n16 : (uint64_t) kBLMilliNitsMax << 16) * 1000 >> 16;
		millinits = millinits < kBLMilliNitsMin ? kBLMilliNitsMin : millinits > kBLMilliNitsMax ? kBLMilliNitsMax : millinits;
		v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
	} else if (value != NULL) {
		v = value->unsigned64BitValue();
		v = v < kBLValueMin ? kBLValueMin : v > kBLValueMax ? kBLValueMax : v;
		millinits = kBLMilliNitsMin + (v - kBLValueMin) * (kBLMilliNitsMax - kBLMilliNitsMin) / (kBLValueMax - kBLValueMin);
	} else {
		snprintf(bl.line + used, sizeof(bl.line) - used, "; nothing taken");
		bl_log();
		return;
	}
	level = (uint32_t) (millinits * kBLLevelFull / kBLMilliNitsMax);
	if (level != redwood_backlight_level) {
		redwood_backlight_level = level;
		MiniIOMobileFramebufferDim(level);
	}
	bl.service->publish(v, millinits);
	/* (read back from the mixer: a burst prints only its last request, which may not have changed the level) */
	alpha = MiniIOMobileFramebufferDimShown();
	if (alpha >= 0) {
		snprintf(shown, sizeof(shown), ", lm0 alpha %d of 1023", alpha);
	} else {
		snprintf(shown, sizeof(shown), ", not darkened");
	}
	snprintf(bl.line + used, sizeof(bl.line) - used, "; value %llu, %llu.%03llu nits, level %u of %u%s", v,
	    millinits / 1000, millinits % 1000, level, kBLLevelFull, shown);
	bl_log();
}

/* brightness and brightness-nits are taken, everything else is only logged; no request fails. */
IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(bl.lock);
	bl_take(dict, "brightness", "brightness-nits");
	IOLockUnlock(bl.lock);
	return kIOReturnSuccess;
}

/* IOMobileFramebuffer::setProperties. A request without IOMFBBrightnessLevel is refused, logged the first times. */
IOReturn
MiniAppleARMBacklightFramebufferProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	static unsigned said;

	if (dict == NULL || bl.service == NULL || OSDynamicCast(OSNumber, dict->getObject("IOMFBBrightnessLevel")) == NULL) {
		if (said++ < 8) {
			char line[256] = "", who[24] = "?";

			if (dict != NULL) {
				list_keys(line, sizeof(line), 0, dict);
			}
			proc_selfname(who, sizeof(who));
			IOLog("MiniIOMobileFramebuffer: %s[%d] sets properties, refused:%s\n", who, proc_selfpid(), line);
		}
		return kIOReturnUnsupported;
	}
	IOLockLock(bl.lock);
	bl_take(dict, NULL, "IOMFBBrightnessLevel");
	IOLockUnlock(bl.lock);
	return kIOReturnSuccess;
}

void
MiniAppleARMBacklightStart(IOService * provider)
{
	AppleARMBacklight * backlight = OSTypeAlloc(AppleARMBacklight);

	bl.lock = IOLockAlloc();
	bl.later = thread_call_allocate(bl_log_later, NULL);
	if (bl.lock == NULL || bl.later == NULL || backlight == NULL || !backlight->init()) {
		OSSafeReleaseNULL(backlight);
		IOLog("MiniAppleARMBacklight: cannot create the service\n");
		return;
	}
	backlight->setProperty("backlight-control", kOSBooleanTrue);
	backlight->publish(kBLValueMax, kBLMilliNitsMax);
	publish_calibration(backlight);
	if (!backlight->attach(provider)) {
		backlight->release();
		IOLog("MiniAppleARMBacklight: cannot attach the service\n");
		return;
	}
	backlight->setName("backlight");
	if (!backlight->start(provider)) {
		backlight->detach(provider);
		backlight->release();
		return;
	}
	bl.service = backlight;         /* (kept: the requests use it) */
	backlight->registerService();
	IOLog("MiniAppleARMBacklight: brightness %u to %u, %u to %u nits, at the top; calibration %u/%u/%u nits, "
	    "marketing table of %lu\n", kBLValueMin, kBLValueMax, kBLMilliNitsMin / 1000, kBLMilliNitsMax / 1000,
	    kBLMilliNitsMin / 1000, kBLMilliNitsMid / 1000, kBLMilliNitsMax / 1000,
	    sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0]));
}

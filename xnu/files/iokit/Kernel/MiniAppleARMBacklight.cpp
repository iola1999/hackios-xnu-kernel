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
 * Every value becomes a value of the panel's brightness range and a level, the fraction of the panel's
 * brightness to show. With boot-arg fb_dcs=4 the value is sent to the panel as DCS 0x51
 * (MiniFramebufferDCS.inc) and the picture is left whole. Otherwise the panel stays as bright as the
 * loader left it, and with boot-arg fb_dim=1 the display controller darkens the picture by the level
 * (MiniFramebufferDim.inc); without either the service answers the same and nothing gets darker. Every
 * request is logged, with all its keys: one line in kBLLogMS at most, and the last of a burst always.
 */

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
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
 * How long an integer-nits request waits for its 16.16 companion before it applies on its own.
 * b5r23 pairs them tens of microseconds apart (median 64 us, 90% within 411 us); 2 ms is far below
 * the panel's frame time and the pace of any ramp, so slider drags and fades are not delayed in
 * practice, while integer-only writes (never seen paired) are applied essentially at once.
 */
#define kBLDedupUS              2000u

/*
 * Boot-arg bl_arb (default 1): one writer to the panel. CoreBrightness hands the brightness to both
 * this service (brightness, brightness-nits, DisplayBrightness) and the framebuffer
 * (IOMFBBrightnessLevel), in different units and at different moments of its control cycle, and both
 * used to land in the panel's DCS 0x51 -- the end of the boot's convergence alternated a few steps
 * up and down, visible as a slight flicker. With arbitration on, the service's first positive
 * request makes it the only writer; IOMFBBrightnessLevel goes on updating the driver's state and the
 * log but no longer reaches the panel. Before that first request the framebuffer writes the panel as
 * before, so the early boot is not left without brightness control; and once the service has asked
 * for the panel off, the framebuffer does not relight it. bl_arb=0 keeps the old both-write behaviour.
 *
 * Boot-arg bl_dedup (default 1): inside the service path, one logical brightness reaches the panel
 * once. CoreBrightness emits DisplayBrightness's NitsPhysical/Nits (whole nits) and brightness-nits
 * (16.16, the same brightness with the fraction) together per control iteration, the integer form
 * first tens of microseconds ahead. Applied as they come, the panel gets the integer-stepped value
 * and then the precise one of every pair (b5r23: up to 17 DBV apart, a visible shimmer). The 16.16
 * form is strictly more accurate, so with dedup on an integer-nits request that matches the whole
 * nits of the last 16.16 value is dropped as a coarser restatement, and one that does not is held
 * back kBLDedupUS: a 16.16 request arriving within the window supersedes it, otherwise it applies
 * late -- integer-only sequences (if any) keep working with a sub-frame delay. bl_dedup=0 applies
 * every request as it comes, the pre-dedup behaviour.
 *
 * Boot-arg bl_trace=1: a "tl: B" line into the loader's log for every brightness request of either
 * path and every DCS write the panel gets, each with the kernel's time in microseconds -- the merged
 * logging below keeps only one line of a 100 ms burst, which is where the alternation used to hide.
 * The variable is shared with MiniFramebufferDCS.inc, which traces the writes themselves.
 */
static TUNABLE(uint32_t, gBLArb, "bl_arb", 1);
static TUNABLE(uint32_t, gBLDedup, "bl_dedup", 1);
extern "C" {
TUNABLE(uint32_t, annibale_bl_trace, "bl_trace", 0);
void annibale_tl(const char * format, ...) __printflike(1, 2);
}

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

/*
 * Inverse of CoreBrightness's 11-point marketing table interpolation (23G90, 0x1e2746810):
 * maps millinits back to the 0..65536 slider domain published in IODisplayParameters["brightness"].
 */
static uint32_t
millinits_to_slider16(uint32_t millinits)
{
	const unsigned n = sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0]);
	const unsigned segments = n - 1;

	if (millinits <= kBLMarketingMilliNits[0]) {
		return 0;
	}
	if (millinits >= kBLMarketingMilliNits[segments]) {
		return 65536u;
	}
	for (unsigned i = 0; i < segments; i++) {
		uint32_t lo = kBLMarketingMilliNits[i];
		uint32_t hi = kBLMarketingMilliNits[i + 1];

		if (millinits <= hi && hi > lo) {
			uint64_t span = hi - lo;
			uint64_t num = ((uint64_t) i * span + (millinits - lo)) * 65536ULL;
			uint64_t den = (uint64_t) segments * span;

			return (uint32_t) ((num + den / 2) / den);
		}
	}
	return 65536u;
}

/* The level the mixers were last given, in 1024ths (nits over kBLMilliNitsMax; full while the panel takes the value). */
extern "C" {
uint32_t annibale_backlight_level = kBLLevelFull;
uint64_t annibale_cpufreq_touched(void);
bool annibale_backlight_load(uint32_t *panel_v_out, uint32_t *millinits_out);
void annibale_backlight_save(uint32_t panel_v, uint32_t millinits);
bool annibale_nvram_kept(void);                                             /* AnnibaleNVRAM.cpp */
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
	thread_call_t           defcall;        /* applies a held-back integer-nits request (bl_dedup) */
	uint64_t                logged;         /* when the last line was printed */
	uint64_t                last_slider_t;  /* timestamp of previous DisplayBrightness update */
	uint64_t                last_slider_pos;/* last seen slider position */
	uint64_t                user_until;     /* deadline until which slider updates are active */
	uint64_t                last_v;         /* current panel brightness value */
	uint64_t                last_millinits; /* current brightness in millinits */
	uint64_t                precise_mn;     /* millinits of the last 16.16 nits request of either path */
	uint64_t                def_v;          /* the held-back request: panel value, millinits, raw key value */
	uint64_t                def_millinits;
	uint64_t                def_raw;
	uint32_t                unlogged;       /* requests since then, not printed yet */
	bool                    initialized;    /* true once boot or user brightness has been applied */
	bool                    slider_initialized; /* true once initial slider position is recorded */
	bool                    nvram_seeded;   /* true once NVRAM holds a restored or saved level */
	bool                    service_active; /* the service has asked for a positive brightness: it alone writes the panel (bl_arb) */
	bool                    service_off;    /* ... and before that it asked for the panel off: the framebuffer does not relight it */
	bool                    precise_valid;  /* precise_mn is meaningful */
	bool                    def_pending;    /* a held-back integer-nits request waits on defcall */
	bool                    def_touch;      /* the held-back request counts as user interaction for NVRAM */
	const char *            def_tkey;       /* its key name (a static string) */
	char                    def_who[24];    /* its requester */
	char                    line[320];      /* the last request, as it is printed */
	char                    def_line[320];  /* the held-back request's line, printed if it applies */
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

/*
 * IODisplayParameters, whole and new each time: user space may be reading the one before.
 * Under useNewBacklightArchitecture, CoreBrightness's DisplayOpen (23G90, 0x1e27444a4) treats
 * brightness as the 0..65536 normalized slider position mapped through backlight-marketing-table,
 * while rawBrightness exposes the linear panel DBV range.
 */
void
AppleARMBacklight::publish(uint64_t value, uint64_t millinits)
{
	OSDictionary * parameters = OSDictionary::withCapacity(3);
	uint64_t pos = millinits_to_slider16((uint32_t) millinits);
	OSDictionary * brightness = range(0, 65536u, pos);
	OSDictionary * raw = range(kBLValueMin, kBLValueMax, value);
	OSDictionary * nits = range(kBLMilliNitsMin, kBLMilliNitsMax, millinits);

	if (parameters != NULL && brightness != NULL && raw != NULL && nits != NULL) {
		parameters->setObject("brightness", brightness);
		parameters->setObject("rawBrightness", raw);
		parameters->setObject("BrightnessMilliNits", nits);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(nits);
	OSSafeReleaseNULL(raw);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
}

/* backlight-calibration-parameters and backlight-marketing-table, once: they do not change. */
static void
publish_calibration(IOService * service)
{
	OSDictionary * calibration = OSDictionary::withCapacity(9);
	uint32_t table[sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0])];
	uint32_t zero = 0;
	OSData * marketing;
	OSData * zero_data = OSData::withBytes(&zero, sizeof(zero));

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
	if (zero_data != NULL) {
		service->setProperty("use-AAP", zero_data);
		service->setProperty("use-cabal", zero_data);
		service->setProperty("use-new-AAP-ramp", zero_data);
	}
	OSSafeReleaseNULL(zero_data);
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
		OSObject * obj = dict->getObject(key);
		OSNumber * n = OSDynamicCast(OSNumber, obj);
		OSDictionary * sub = OSDynamicCast(OSDictionary, obj);

		if (n != NULL) {
			used += (size_t) snprintf(line + used, size - used, " %s=%llu", key->getCStringNoCopy(), n->unsigned64BitValue());
		} else if (sub != NULL) {
			used += (size_t) snprintf(line + used, size - used, " %s={", key->getCStringNoCopy());
			used = list_keys(line, size, used < size ? used : size, sub);
			if (used < size) {
				used += (size_t) snprintf(line + used, size - used, " }");
			}
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
 * VALUE_KEY (a value of the raw panel range, none for the framebuffer). DisplayBrightness's
 * NitsPhysical/Nits are nits (CoreBrightness 23G90, 0x1e273eb08). DisplayBrightnessNitsNVRAM is
 * iBoot's boot-logo NVRAM seed (0x1e274544c) and never changes live panel brightness.
 * Slider position records interaction; CoreBrightness supplies the mapped nits.
 * The result is a panel value, nits, and the level given to the mixers when it changes.
 */
static inline uint64_t
bl_micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

/*
 * With arbitration on (bl_arb), may the writer of this request change the panel? The service always
 * may; the framebuffer may until the service's first positive request, and not once the service has
 * asked for the panel off either. A request that may not is still state and log, nothing physical.
 */
static bool
bl_may_write(bool from_service)
{
	return from_service || gBLArb == 0 || (!bl.service_active && !bl.service_off);
}

/*
 * A positive brightness request takes effect: driver state, the arbitration activation, the NVRAM
 * copy of user brightnesses, the panel and the mixers, the trace line, the published parameters and
 * the merged log line. The request's line prefix is in bl.line. DEFERRED marks the trace of one
 * that was held back by bl_dedup. With bl.lock held.
 */
static void
bl_commit(uint64_t v, uint64_t millinits, const char * tkey, uint64_t traw, const char * who,
    bool from_service, bool touch_save, bool deferred)
{
	size_t used = strlen(bl.line);
	uint32_t level;
	int alpha;
	char shown[32];

	bl.last_v = v;
	bl.last_millinits = millinits;
	bl.initialized = true;
	if (from_service) {
		if (!bl.service_active && gBLArb != 0) {
			IOLog("MiniAppleARMBacklight: the first service brightness (%llu.%03llu nits): "
			    "the panel takes the service's alone from here\n", millinits / 1000, millinits % 1000);
		}
		bl.service_active = true;
		bl.service_off = false;
	}
	if (touch_save) {
		annibale_backlight_save((uint32_t) v, (uint32_t) millinits);
		bl.nvram_seeded = true;
		bl.service->setProperty("annibale-bl-restored", kOSBooleanTrue);
	}
	level = (uint32_t) (millinits * kBLLevelFull / kBLMilliNitsMax);
	bool may = bl_may_write(from_service);
	uint32_t want_before = MiniIOMobileFramebufferDCSWanted();
	if (may) {
		/* with fb_dcs=4 the panel takes v itself, and the mixers are given the full level */
		uint32_t mixed = MiniIOMobileFramebufferDCS((uint32_t) v) ? kBLLevelFull : level;

		if (mixed != annibale_backlight_level) {
			annibale_backlight_level = mixed;
			MiniIOMobileFramebufferDim(mixed);
		}
	}
	if (annibale_bl_trace != 0) {
		annibale_tl("tl: B %s %s %s=%llu mn %llu dbv %llu want %u>%u %s at %llu\n",
		    from_service ? "svc" : "mfb", who, tkey != NULL ? tkey : "-", traw, millinits, v,
		    want_before, MiniIOMobileFramebufferDCSWanted(),
		    deferred ? "dwr" : may ? "wr" : "blk",
		    bl_micros(mach_absolute_time()));
	}
	bl.service->publish(v, millinits);
	/* (read back from the mixer: a burst prints only its last request, which may not have changed the level) */
	alpha = MiniIOMobileFramebufferDimShown();
	if (alpha >= 0) {
		snprintf(shown, sizeof(shown), ", lm0 alpha %d of 1023", alpha);
	} else {
		snprintf(shown, sizeof(shown), ", not darkened");
	}
	snprintf(bl.line + used, sizeof(bl.line) - used, "; value %llu, %llu.%03llu nits, level %u of %u%s%s", v,
	    millinits / 1000, millinits % 1000, level, kBLLevelFull, shown, may ? "" : ", not written");
	bl_log();
}

/* With bl.lock held: a held-back integer-nits request is superseded; its trace pairs with the "def" line. */
static void
bl_def_drop(bool from_service, const char * who)
{
	bl.def_pending = false;
	(void) thread_call_cancel(bl.defcall);
	if (annibale_bl_trace != 0) {
		annibale_tl("tl: B %s %s - defdrop at %llu\n",
		    from_service ? "svc" : "mfb", who, bl_micros(mach_absolute_time()));
	}
}

/* kBLDedupUS after the request, when no 16.16 request superseded it: the held-back request applies. */
static void
bl_def_fire(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(bl.lock);
	if (bl.def_pending) {
		uint64_t v = bl.def_v, millinits = bl.def_millinits, raw = bl.def_raw;
		const char * tkey = bl.def_tkey;
		bool touch = bl.def_touch;
		char who[24];

		bl.def_pending = false;
		strlcpy(who, bl.def_who, sizeof(who));
		memcpy(bl.line, bl.def_line, sizeof(bl.line));
		bl_commit(v, millinits, tkey, raw, who, true, touch, true);
	}
	IOLockUnlock(bl.lock);
}

static void
bl_take(OSDictionary * dict, const char * value_key, const char * nits_key)
{
	OSNumber * nits = OSDynamicCast(OSNumber, dict->getObject(nits_key));
	OSNumber * value = value_key != NULL ? OSDynamicCast(OSNumber, dict->getObject(value_key)) : NULL;
	OSDictionary * db_dict = OSDynamicCast(OSDictionary, dict->getObject("DisplayBrightness"));
	bool plain_nits = false;
	bool from_service = value_key != NULL;
	uint64_t now = mach_absolute_time(), active_win;
	uint64_t v, millinits;
	char who[24] = "?";
	size_t used;

	const char * tkey = NULL;
	uint64_t traw = 0;
	bool slider_moved = false;
	bool touch_recent = false;
	uint64_t slider_pos = 0;
	if (value_key != NULL && nits == NULL && value == NULL) {
		if (db_dict != NULL) {
			nits = OSDynamicCast(OSNumber, db_dict->getObject("NitsPhysical"));
			tkey = "NitsPhysical";
			if (nits == NULL) {
				nits = OSDynamicCast(OSNumber, db_dict->getObject("Nits"));
				tkey = "Nits";
			}
			plain_nits = nits != NULL;
		}
	}
	{
		uint64_t touch_win, touched = annibale_cpufreq_touched();
		nanoseconds_to_absolutetime(400 * NSEC_PER_MSEC, &active_win);
		nanoseconds_to_absolutetime(3000 * NSEC_PER_MSEC, &touch_win);
		if (touched != 0 && now >= touched && now - touched <= touch_win) {
			touch_recent = true;
		} else if (bl.user_until != 0 && now <= bl.user_until) {
			touch_recent = true;
		}
	}
	if (value_key != NULL) {
		OSNumber * slider = OSDynamicCast(OSNumber, dict->getObject("DisplayBrightnessSliderPosition"));
		if (slider == NULL && db_dict != NULL) {
			slider = OSDynamicCast(OSNumber, db_dict->getObject("DisplayBrightnessSliderPosition"));
		}
		if (slider != NULL) {
			uint64_t pos = slider->unsigned64BitValue();
			slider_pos = pos > 65536 ? 65536 : pos;
			if (!bl.slider_initialized) {
				bl.slider_initialized = true;
				bl.last_slider_pos = slider_pos;
			} else if (!touch_recent) {
				bl.last_slider_pos = slider_pos;
			} else {
				uint64_t diff = slider_pos > bl.last_slider_pos ?
				    slider_pos - bl.last_slider_pos : bl.last_slider_pos - slider_pos;
				if (diff >= 64) {
					bl.user_until = now + active_win;
					bl.last_slider_pos = slider_pos;
					slider_moved = true;
				}
			}
		}
	}

	proc_selfname(who, sizeof(who));
	used = (size_t) snprintf(bl.line, sizeof(bl.line), "%s[%d] to the %s:", who, proc_selfpid(),
	    value_key != NULL ? "backlight" : "framebuffer");
	used = list_keys(bl.line, sizeof(bl.line), used < sizeof(bl.line) ? used : sizeof(bl.line), dict);
	if (nits != NULL) {
		uint64_t n16 = nits->unsigned64BitValue();
		int64_t s16 = (int64_t) n16;

		if (tkey == NULL) {
			tkey = nits_key;
		}
		traw = n16;
		if (s16 <= 0) {
			bool may = bl_may_write(from_service);
			uint32_t want_before = MiniIOMobileFramebufferDCSWanted();

			if (from_service) {
				bl.service_off = true;
			}
			if (may && bl.def_pending) {
				/* the panel goes dark: a held-back positive request must not relight it */
				bl_def_drop(from_service, who);
			}
			/*
			 * The relight after a dark panel may restate the brightness from before it in whole
			 * nits; with the 16.16 reference forgotten it cannot be mistaken for a duplicate and
			 * suppressed, which would leave the panel dark.
			 */
			bl.precise_valid = false;
			if (may && !MiniIOMobileFramebufferDCS(0) && annibale_backlight_level != 0) {
				annibale_backlight_level = 0;
				MiniIOMobileFramebufferDim(0);
			}
			snprintf(bl.line + used, sizeof(bl.line) - used,
			    "; panel off (%lld), keeping %llu.%03llu nits%s",
			    (long long) s16, bl.last_millinits / 1000, bl.last_millinits % 1000,
			    may ? "" : ", not written");
			bl_log();
			if (annibale_bl_trace != 0) {
				annibale_tl("tl: B %s %s %s=%lld off want %u>%u %s at %llu\n",
				    from_service ? "svc" : "mfb", who, tkey, (long long) s16,
				    want_before, MiniIOMobileFramebufferDCSWanted(), may ? "off" : "blk",
				    bl_micros(mach_absolute_time()));
			}
			return;
		}

		/* CoreBrightness's DisplayBrightness dictionary uses nits; the other keys use 16.16. */
		if (plain_nits) {
			millinits = (n16 < kBLMilliNitsMax / 1000 ? n16 : kBLMilliNitsMax / 1000) * 1000;
		} else {
			millinits = (n16 < fixed_nits(kBLMilliNitsMax) ? n16 : fixed_nits(kBLMilliNitsMax)) * 1000 >> 16;
		}
		millinits = millinits < kBLMilliNitsMin ? kBLMilliNitsMin : millinits > kBLMilliNitsMax ? kBLMilliNitsMax : millinits;
		v = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);
	} else if (value != NULL) {
		v = value->unsigned64BitValue();
		tkey = value_key;
		traw = v;
		if (v == 0) {
			bool may = bl_may_write(from_service);
			uint32_t want_before = MiniIOMobileFramebufferDCSWanted();

			if (from_service) {
				bl.service_off = true;
			}
			if (may && bl.def_pending) {
				/* the panel goes dark: a held-back positive request must not relight it */
				bl_def_drop(from_service, who);
			}
			/*
			 * The relight after a dark panel may restate the brightness from before it in whole
			 * nits; with the 16.16 reference forgotten it cannot be mistaken for a duplicate and
			 * suppressed, which would leave the panel dark.
			 */
			bl.precise_valid = false;
			if (may && !MiniIOMobileFramebufferDCS(0) && annibale_backlight_level != 0) {
				annibale_backlight_level = 0;
				MiniIOMobileFramebufferDim(0);
			}
			snprintf(bl.line + used, sizeof(bl.line) - used,
			    "; panel off (value 0), keeping %llu.%03llu nits%s",
			    bl.last_millinits / 1000, bl.last_millinits % 1000,
			    may ? "" : ", not written");
			bl_log();
			if (annibale_bl_trace != 0) {
				annibale_tl("tl: B %s %s %s=0 off want %u>%u %s at %llu\n",
				    from_service ? "svc" : "mfb", who, tkey,
				    want_before, MiniIOMobileFramebufferDCSWanted(), may ? "off" : "blk",
				    bl_micros(mach_absolute_time()));
			}
			return;
		}
		v = v < kBLValueMin ? kBLValueMin : v > kBLValueMax ? kBLValueMax : v;
		millinits = kBLMilliNitsMin + (v - kBLValueMin) * (kBLMilliNitsMax - kBLMilliNitsMin) / (kBLValueMax - kBLValueMin);
	} else {
		snprintf(bl.line + used, sizeof(bl.line) - used, "; nothing taken");
		bl_log();
		if (annibale_bl_trace != 0) {
			annibale_tl("tl: B %s %s - skip at %llu\n",
			    from_service ? "svc" : "mfb", who, bl_micros(mach_absolute_time()));
		}
		return;
	}
	/*
	 * The request's brightness is in v and millinits. A 16.16 nits request applies at once and is
	 * the reference the integer-nits dedup goes by; a bare brightness request applies at once too.
	 * Either supersedes a held-back integer-nits request: they are the newer word on the brightness.
	 */
	if (!plain_nits) {
		if (nits != NULL) {
			bl.precise_mn = millinits;
			bl.precise_valid = true;
		}
		if (bl.def_pending) {
			bl_def_drop(from_service, who);
		}
		bl_commit(v, millinits, tkey, traw, who, from_service, touch_recent || slider_moved, false);
		return;
	}
	if (gBLDedup != 0) {
		uint32_t want_now = MiniIOMobileFramebufferDCSWanted();

		if (bl.precise_valid && bl.precise_mn / 1000 == traw) {
			/* the last 16.16 request's brightness restated in whole nits: nothing new to apply */
			if (annibale_bl_trace != 0) {
				annibale_tl("tl: B %s %s %s=%llu mn %llu dbv %llu want %u>%u dup at %llu\n",
				    from_service ? "svc" : "mfb", who, tkey, traw, millinits, v,
				    want_now, want_now, bl_micros(mach_absolute_time()));
			}
			return;
		}
		/*
		 * Whole nits that do not match the last 16.16 request: the brightness crossed into another
		 * integer, in which case the pair's 16.16 half follows within tens of microseconds -- or
		 * an integer-only writer, in which case nothing follows. Hold the request back kBLDedupUS
		 * so the precise half wins where there is one; the newest hold replaces any earlier one.
		 */
		bl.def_v = v;
		bl.def_millinits = millinits;
		bl.def_raw = traw;
		bl.def_tkey = tkey;
		bl.def_touch = touch_recent || slider_moved;
		strlcpy(bl.def_who, who, sizeof(bl.def_who));
		memcpy(bl.def_line, bl.line, sizeof(bl.def_line));
		bl.def_pending = true;
		{
			uint64_t interval;

			nanoseconds_to_absolutetime(kBLDedupUS * (uint64_t) NSEC_PER_USEC, &interval);
			(void) thread_call_cancel(bl.defcall);
			thread_call_enter_delayed(bl.defcall, now + interval);
		}
		if (annibale_bl_trace != 0) {
			annibale_tl("tl: B %s %s %s=%llu mn %llu dbv %llu want %u>%u def at %llu\n",
			    from_service ? "svc" : "mfb", who, tkey, traw, millinits, v,
			    want_now, want_now, bl_micros(mach_absolute_time()));
		}
		return;
	}
	bl_commit(v, millinits, tkey, traw, who, from_service, touch_recent || slider_moved, false);
}

/*
 * brightness, brightness-nits, and DisplayBrightness drive live panel brightness.
 * DisplayBrightnessNitsNVRAM (AppleARMBacklight::setPropertiesGated, 0xfffffe00084b3d6c) only
 * seeds NVRAM when no user level is stored yet, leaving the active panel and IODisplayParameters untouched.
 */
IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(bl.lock);
	OSNumber * nvram_nits = OSDynamicCast(OSNumber, dict->getObject("DisplayBrightnessNitsNVRAM"));
	if (nvram_nits != NULL &&
	    dict->getObject("brightness-nits") == NULL &&
	    dict->getObject("brightness") == NULL &&
	    dict->getObject("DisplayBrightness") == NULL) {
		char who[24] = "?";
		size_t used;
		int64_t s16 = (int64_t) nvram_nits->unsigned64BitValue();
		bool seeded = false;

		if (!bl.nvram_seeded && s16 > 0) {
			uint64_t n16 = (uint64_t) s16;
			uint64_t mn = (n16 < fixed_nits(kBLMilliNitsMax) ? n16 : fixed_nits(kBLMilliNitsMax)) * 1000 >> 16;
			mn = mn < kBLMilliNitsMin ? kBLMilliNitsMin : mn > kBLMilliNitsMax ? kBLMilliNitsMax : mn;
			uint64_t seed_v = kBLValueMin + (mn - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) /
			    (kBLMilliNitsMax - kBLMilliNitsMin);
			annibale_backlight_save((uint32_t) seed_v, (uint32_t) mn);
			bl.nvram_seeded = true;
			seeded = true;
		}
		proc_selfname(who, sizeof(who));
		used = (size_t) snprintf(bl.line, sizeof(bl.line), "%s[%d] to the backlight:", who, proc_selfpid());
		used = list_keys(bl.line, sizeof(bl.line), used < sizeof(bl.line) ? used : sizeof(bl.line), dict);
		snprintf(bl.line + used, sizeof(bl.line) - used,
		    "; NVRAM seed %s, keeping %llu.%03llu nits",
		    seeded ? "stored" : "ignored (user brightness kept)",
		    bl.last_millinits / 1000, bl.last_millinits % 1000);
		bl_log();
		IOLockUnlock(bl.lock);
		return kIOReturnSuccess;
	}
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

/*
 * The brightness across restarts, as AppleARMBacklight keeps it: NVRAM's backlight-level (the panel's
 * value, "%d") and backlight-nits (16.16 nits, "0x%08x"). They are in IODTNVRAM's dictionary at once and
 * in darwin_nvram after its next sync (AnnibaleNVRAM.cpp). MiniFramebufferDCS.inc also reads it, for
 * the panel at start.
 */
static bool
nvram_number(const char * name, uint64_t * out)
{
	char text[24];
	unsigned int len = sizeof(text), i = 0, base = 10;
	uint64_t n = 0;

	if (!PEReadNVRAMProperty(name, text, &len) || len == 0 || len > sizeof(text)) {
		return false;
	}
	if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		base = 16;
		i = 2;
	}
	for (; i < len && text[i] != 0; i++) {
		char c = text[i];
		unsigned int d = c >= '0' && c <= '9' ? (unsigned int)(c - '0') :
		    c >= 'a' && c <= 'f' ? (unsigned int)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned int)(c - 'A' + 10) : 16;

		if (d >= base || n > 0xffffffffULL) {
			return false;
		}
		n = n * base + d;
	}
	*out = n;
	return true;
}

bool
annibale_backlight_load(uint32_t * panel_v_out, uint32_t * millinits_out)
{
	uint64_t v, n16;

	if (!nvram_number("backlight-level", &v) || !nvram_number("backlight-nits", &n16) || v == 0 || n16 == 0 ||
	    v > 0xffffffffULL || n16 > 0xffffffffULL) {
		return false;
	}
	if (panel_v_out != NULL) {
		*panel_v_out = (uint32_t) v;
	}
	if (millinits_out != NULL) {
		*millinits_out = (uint32_t) ((n16 * 1000 + 32768) >> 16);
	}
	return true;
}

/* With bl.lock held. */
void
annibale_backlight_save(uint32_t panel_v, uint32_t millinits)
{
	static uint32_t saved_v, saved_mn;
	IORegistryEntry * options;
	OSDictionary * dict;
	OSString * level = NULL, * nits = NULL;
	char text[16];

	if (panel_v == 0 || millinits == 0) {
		return;
	}
	if (panel_v == saved_v && millinits == saved_mn) {
		return;
	}
	options = IORegistryEntry::fromPath("/options", gIODTPlane);
	dict = OSDictionary::withCapacity(2);
	snprintf(text, sizeof(text), "%u", panel_v);
	level = OSString::withCString(text);
	snprintf(text, sizeof(text), "0x%08x", (uint32_t) ((((uint64_t) millinits << 16) + 500) / 1000));
	nits = OSString::withCString(text);
	if (options != NULL && dict != NULL && level != NULL && nits != NULL) {
		dict->setObject("backlight-level", level);
		dict->setObject("backlight-nits", nits);
		if (options->setProperties(dict) == kIOReturnSuccess) {
			saved_v = panel_v;
			saved_mn = millinits;
		} else {
			IOLog("MiniAppleARMBacklight: NVRAM does not take backlight-level %s\n", level->getCStringNoCopy());
		}
	}
	OSSafeReleaseNULL(nits);
	OSSafeReleaseNULL(level);
	OSSafeReleaseNULL(dict);
	OSSafeReleaseNULL(options);
}

void
MiniAppleARMBacklightStart(IOService * provider)
{
	AppleARMBacklight * backlight = OSTypeAlloc(AppleARMBacklight);
	uint32_t saved_v = kBLValueMax, saved_mn = kBLMilliNitsMax;
	uint32_t boot_level, boot_mixed;

	bl.lock = IOLockAlloc();
	bl.later = thread_call_allocate(bl_log_later, NULL);
	bl.defcall = thread_call_allocate(bl_def_fire, NULL);
	if (bl.lock == NULL || bl.later == NULL || bl.defcall == NULL || backlight == NULL || !backlight->init()) {
		OSSafeReleaseNULL(backlight);
		IOLog("MiniAppleARMBacklight: cannot create the service\n");
		return;
	}
	bool restored = annibale_backlight_load(&saved_v, &saved_mn) &&
	    saved_v >= kBLValueMin && saved_v <= kBLValueMax &&
	    saved_mn >= kBLMilliNitsMin && saved_mn <= kBLMilliNitsMax;
	if (!restored) {
		saved_v = kBLValueMax;
		saved_mn = kBLMilliNitsMax;
	}
	bl.last_v = saved_v;
	bl.last_millinits = saved_mn;
	bl.nvram_seeded = restored;
	boot_level = (uint32_t) (bl.last_millinits * kBLLevelFull / kBLMilliNitsMax);
	boot_mixed = MiniIOMobileFramebufferDCS((uint32_t) bl.last_v) ? kBLLevelFull : boot_level;
	if (boot_mixed != annibale_backlight_level) {
		annibale_backlight_level = boot_mixed;
		MiniIOMobileFramebufferDim(boot_mixed);
	}
	backlight->setProperty("backlight-control", kOSBooleanTrue);
	backlight->setProperty("annibale-bl-restored", restored ? kOSBooleanTrue : kOSBooleanFalse);
	backlight->publish(bl.last_v, bl.last_millinits);
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
	bl.initialized = true;
	bl.slider_initialized = false;
	bl.last_slider_pos = 0;
	bl.user_until = 0;
	backlight->registerService();
	IOLog("MiniAppleARMBacklight: brightness %u to %u, %u to %u nits, start %llu (%llu.%03llu nits); calibration %u/%u/%u nits, "
	    "marketing table of %lu\n", kBLValueMin, kBLValueMax, kBLMilliNitsMin / 1000, kBLMilliNitsMax / 1000,
	    bl.last_v, bl.last_millinits / 1000, bl.last_millinits % 1000,
	    kBLMilliNitsMin / 1000, kBLMilliNitsMid / 1000, kBLMilliNitsMax / 1000,
	    sizeof(kBLMarketingMilliNits) / sizeof(kBLMarketingMilliNits[0]));
}

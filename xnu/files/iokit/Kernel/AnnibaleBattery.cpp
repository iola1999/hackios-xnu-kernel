/*
 * AnnibaleBattery: the phone's battery as iOS looks for it, an IOPMPowerSource.
 *
 * powerd finds every service of the class IOPMPowerSource (it calls the first "InternalBattery-0"),
 * listens for its kIOPMMessageBatteryStatusHasChanged and then reads its properties: whether a
 * battery is installed, CurrentCapacity and MaxCapacity (without both it gives up on the battery),
 * whether a charger is connected and charging, voltage, current, cycles, the charger's details.
 * From them it makes the power sources list (IOPSCopyPowerSourcesInfo) and posts its notifications,
 * and everything else follows from that: the status bar's battery, the lock screen's "Charging",
 * the Batteries widget, Settings > Battery.
 *
 * What the values are comes from the audio DSP's battery manager (bsd/dev/annibale_adsp.c,
 * with adsp=1, unless battmgr=0), by way of bsd/dev/annibale/annibale_battery_state.c, which calls
 * AnnibaleBatteryReport() with every reading. Nothing here asks the charger anything or tells it
 * anything.
 *
 * The service itself is made at boot by AnnibaleBatteryStart() (from annibale_battery_init, next
 * to the AppleSMC stub), publishing skeleton BatteryData/ChargerData dictionaries before any real
 * reading exists. batteryintelligenced's first XPC activity can run before the DSP's first reading
 * arrives; when it found no power source at all, the absent properties turned into a nil member
 * and an NSInvalidArgumentException while it built @{ smcData: ..., systemState: ... }. An early
 * service with the empty dictionaries closes that window (its once-per-boot abort, then stable on
 * relaunch).
 *
 * As on Apple's newer batteries, CurrentCapacity is the percentage that the fuel gauge shows (and
 * MaxCapacity 100); the charges in mAh are AppleRawCurrentCapacity, AppleRawMaxCapacity (what the
 * gauge has learnt a full charge to be), NominalChargeCapacity and DesignCapacity.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>

/* The same as struct annibale_battery_state in bsd/sys/annibale_input.h, which is not seen from here. */
struct annibale_battery_state {
	uint32_t size;
	int32_t percent_hundredths;
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths;
	int32_t present, external, status;
	int32_t charge_uah, full_uah, design_uah, cycles;
	int32_t usb_type;
	int32_t usb_voltage_uv, usb_current_ua;
};

extern "C" void AnnibaleBatteryReport(const void * state, unsigned size);
extern "C" bool AnnibaleBatteryStart(void);

class AnnibaleBattery : public IOPMPowerSource
{
	OSDeclareDefaultStructors(AnnibaleBattery);

public:
	void report(const struct annibale_battery_state * b);
	void placeholders(void);

private:
	void number(const char * key, long long value);
	void flag(const char * key, bool value);
	void object(const char * key, OSObject * value);
	void dicts(int32_t cycles);
};

OSDefineMetaClassAndStructors(AnnibaleBattery, IOPMPowerSource);

static AnnibaleBattery * battery;

void
AnnibaleBattery::number(const char * key, long long value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);
	OSNumber * n = OSNumber::withNumber((unsigned long long) value, 32);

	if (symbol != NULL && n != NULL) {
		setPSProperty(symbol, n);
	}
	OSSafeReleaseNULL(n);
	OSSafeReleaseNULL(symbol);
}

void
AnnibaleBattery::flag(const char * key, bool value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
	}
	OSSafeReleaseNULL(symbol);
}

void
AnnibaleBattery::object(const char * key, OSObject * value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL && value != NULL) {
		setPSProperty(symbol, value);
	}
	OSSafeReleaseNULL(symbol);
}

/*
 * What iOS calls the charger by (IOPSKeysPrivate.h's kIOPSFamilyCode*, iokit_family_err(sub_iokit_usb, n)),
 * from Linux's POWER_SUPPLY_USB_TYPE_*, as the DSP gives it.
 */
static unsigned
family_code(int usb_type)
{
	switch (usb_type) {
	case 1: return 0xe0004000;      /* SDP, a computer's port: kIOPSFamilyCodeUSBHost */
	case 2: return 0xe0004004;      /* DCP, a charger: kIOPSFamilyCodeUSBChargingPortDedicated */
	case 3: return 0xe0004005;      /* CDP, a computer's charging port: kIOPSFamilyCodeUSBChargingPortDownstream */
	case 5: return 0xe0004009;      /* USB-C: kIOPSFamilyCodeUSBCTypeC */
	case 6: case 7: case 8: return 0xe000400a;      /* USB Power Delivery: kIOPSFamilyCodeUSBCPD */
	default: return 0xe0004007;     /* kIOPSFamilyCodeUSBUnknown */
	}
}

static void
set_u32(OSDictionary * dict, const char * key, uint32_t value)
{
	OSNumber * n = OSNumber::withNumber((unsigned long long) value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

static void
set_data(OSDictionary * dict, const char * key, const void * bytes, unsigned len)
{
	OSData * d = OSData::withBytes(bytes, len);

	if (d != NULL) {
		dict->setObject(key, d);
		d->release();
	}
}

/*
 * batteryintelligenced pulls BatteryData/ChargerData out of the registry
 * properties (IORegistryEntryCreateCFProperties on the first IOPMPowerSource
 * match): BatteryAlgorithmsInit requires non-empty BatteryData, ChargerData, and
 * BatteryData["LifetimeData"], and OnDeviceACAM (BatteryAlgorithms.framework)
 * expects TimeAtHighSoc as NSData (4-byte uint32 bins) and VacVoltageLimit in
 * [4335, 4505] mV.
 */
void
AnnibaleBattery::dicts(int32_t cycles)
{
	OSDictionary * battData = OSDictionary::withCapacity(4);
	OSDictionary * lifetime = OSDictionary::withCapacity(3);
	OSDictionary * chargerData = OSDictionary::withCapacity(2);
	if (battData != NULL && lifetime != NULL && chargerData != NULL) {
		uint32_t cycleCount = cycles > 0 ? (uint32_t) cycles : 0u;
		uint32_t teqZero = 0u;
		if (cycles > 0) {
			set_u32(battData, "CycleCount", cycleCount);
		}
		set_u32(lifetime, "TotalOperatingTime", 0u);
		set_u32(lifetime, "CycleCountLastQmax", cycleCount);
		set_data(lifetime, "TimeAtHighSoc", &teqZero, sizeof(teqZero));
		set_u32(chargerData, "VacVoltageLimit", 4400u);
		battData->setObject("LifetimeData", lifetime);
		object("BatteryData", battData);
		object("ChargerData", chargerData);
	}
	OSSafeReleaseNULL(battData);
	OSSafeReleaseNULL(lifetime);
	OSSafeReleaseNULL(chargerData);
}

/* The empty BatteryData/ChargerData skeletons for the service made at boot, before the first reading. */
void
AnnibaleBattery::placeholders(void)
{
	dicts(0);
}

void
AnnibaleBattery::report(const struct annibale_battery_state * b)
{
	int percent = (b->percent_hundredths + 50) / 100;
	/*
	 * Charging means plugged in and the battery not feeding the phone, as an iPhone and Android show it:
	 * near full the charger stops (POWER_SUPPLY_STATUS_NOT_CHARGING) until the recharge threshold, and
	 * with only STATUS_CHARGING counted the status bar lost its bolt at 99% on a charger. On the K90
	 * both fields come from the fuel gauge's current (annibale_battery.c).
	 */
	bool charging = b->external && b->status != 2;          /* not POWER_SUPPLY_STATUS_DISCHARGING */

	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	setBatteryInstalled(b->present != 0);
	setExternalConnected(b->external != 0);
	setExternalChargeCapable(b->external != 0);
	setIsCharging(charging);
	flag("FullyCharged", b->external && (b->status == 4 || percent == 100));    /* POWER_SUPPLY_STATUS_FULL */
	flag("AppleRawExternalConnected", b->external != 0);
	setCurrentCapacity((unsigned) percent);
	setMaxCapacity(100);
	setVoltage((unsigned) (b->voltage_uv / 1000));
	setAmperage(b->current_ua / 1000);
	number("InstantAmperage", b->current_ua / 1000);
	number("Temperature", b->temperature_hundredths);
	if (b->charge_uah > 0) {
		number("AppleRawCurrentCapacity", b->charge_uah / 1000);
	}
	if (b->full_uah > 0) {
		number("AppleRawMaxCapacity", b->full_uah / 1000);
		number("NominalChargeCapacity", b->full_uah / 1000);
	}
	if (b->design_uah > 0) {
		number("DesignCapacity", b->design_uah / 1000);
	}
	if (b->cycles > 0) {
		setCycleCount((unsigned) b->cycles);
	}

	dicts(b->cycles);

	if (b->external) {
		OSDictionary * adapter = OSDictionary::withCapacity(6);
		long long mv = b->usb_voltage_uv / 1000, ma = b->usb_current_ua / 1000;
		OSNumber * n;

		if (adapter != NULL) {
			if ((n = OSNumber::withNumber((unsigned long long) ((mv * ma + 500000) / 1000000), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsWattsKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) ma, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) mv, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsVoltage, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) family_code(b->usb_type), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
				n->release();
			}
			adapter->setObject("IsWireless", kOSBooleanFalse);
			object(kIOPMPSAdapterDetailsKey, adapter);
			adapter->release();
		}
	} else {
		OSDictionary * none = OSDictionary::withCapacity(1);
		OSNumber * n = OSNumber::withNumber(0ULL, 32);    /* kIOPSFamilyCodeDisconnected */

		if (none != NULL && n != NULL) {
			none->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
			object(kIOPMPSAdapterDetailsKey, none);
		}
		OSSafeReleaseNULL(n);
		OSSafeReleaseNULL(none);
	}
	updateStatus();
}

static AnnibaleBattery *
make_service(void)
{
	AnnibaleBattery * made = OSTypeAlloc(AnnibaleBattery);
	IOService * platform = IOService::getPlatform();

	if (platform == NULL || made == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		return NULL;
	}
	made->placeholders();       /* the dictionaries are there before anybody finds the service */
	if (!made->attach(platform)) {
		made->release();
		return NULL;
	}
	made->setName("AnnibaleBattery");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return NULL;
	}
	made->registerService();
	return made;
}

/* At boot (annibale_battery_init), so the power source exists before daemons ask for it; readings fill in later. */
bool
AnnibaleBatteryStart(void)
{
	if (battery == NULL) {
		battery = make_service();
	}
	return battery != NULL;
}

/* With every reading of the battery (bsd/dev/annibale/annibale_battery_state.c). One caller at a time. */
void
AnnibaleBatteryReport(const void * state, unsigned size)
{
	const struct annibale_battery_state * b = (const struct annibale_battery_state *) state;

	if (size != sizeof(struct annibale_battery_state) || b->size != sizeof(struct annibale_battery_state)) {
		static bool said;

		if (!said) {
			said = true;
			IOLog("AnnibaleBattery: the battery's state is %u bytes, not %lu: bsd/sys/annibale_input.h and this file disagree\n",
			    size, sizeof(struct annibale_battery_state));
		}
		return;
	}
	if (battery == NULL && !AnnibaleBatteryStart()) {
		return;
	}
	battery->report(b);
	static bool announced;
	if (!announced) {
		announced = true;
		IOLog("AnnibaleBattery: the battery's power source is up: %d%%, %s\n", (b->percent_hundredths + 50) / 100,
		    b->external ? (b->status == 1 ? "charging" : "on a charger, not charging") : "not on a charger");
	}
	/* each change of charger or charging state, to tell a charger that drops out from one that stops charging */
	static int32_t external = -1, status = -1;
	if (b->external != external || b->status != status) {
		external = b->external;
		status = b->status;
		IOLog("AnnibaleBattery: %d%%, %s, status %d, %d mA; USB type %d, %d mV, %d mA\n", (b->percent_hundredths + 50) / 100,
		    b->external ? "on a charger" : "not on a charger", b->status, b->current_ua / 1000, b->usb_type,
		    b->usb_voltage_uv / 1000, b->usb_current_ua / 1000);
	}
}

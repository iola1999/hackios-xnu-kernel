/*
 * AnnibaleBT: the IOKit service by which iOS's bluetoothd finds the Bluetooth UART.
 *
 * bluetoothd looks up IOServiceNameMatching("bluetooth") (or "marconi-bt") in FindLocalMacAddress
 * and searches for the 6-byte "local-mac-address" property in the IODeviceTree plane; without a
 * service and a non-zero address it logs "No bluetooth on this device" and exits before opening
 * /dev/uart.bluetooth or connecting com.apple.uart.sk.bluetooth.
 */
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

extern "C" void AnnibaleBTStart(const char * bsdname, const uint8_t * address);
extern "C" void AnnibaleBTSetAddress(const uint8_t * address);
extern "C" void AnnibaleBTSetPowerState(bool on);
extern "C" void AnnibaleDiskStart(uint64_t size_bytes);

class AnnibaleBT : public IOService
{
	OSDeclareDefaultStructors(AnnibaleBT);
};

OSDefineMetaClassAndStructors(AnnibaleBT, IOService);

class IOMedia : public IOService
{
	OSDeclareDefaultStructors(IOMedia);
};

OSDefineMetaClassAndStructors(IOMedia, IOService);

class AnnibaleDisk : public IOMedia
{
	OSDeclareDefaultStructors(AnnibaleDisk);
};

OSDefineMetaClassAndStructors(AnnibaleDisk, IOMedia);

class AppleAPFSContainer : public IOService
{
	OSDeclareDefaultStructors(AppleAPFSContainer);
};

OSDefineMetaClassAndStructors(AppleAPFSContainer, IOService);

class AppleAPFSVolume : public IOMedia
{
	OSDeclareDefaultStructors(AppleAPFSVolume);
};

OSDefineMetaClassAndStructors(AppleAPFSVolume, IOMedia);

static AnnibaleBT * gAnnibaleBT;
static AnnibaleDisk * gAnnibaleDisk;

static void
bt_set_address(AnnibaleBT * bt, const uint8_t * address)
{
	OSData * mac = OSData::withBytes(address, 6);

	if (mac == NULL) {
		return;
	}
	bt->setProperty("local-mac-address", mac);
	bt->setProperty("bluetooth-mac", mac);
	bt->setProperty("Local-BD-ADDR", mac);
	mac->release();
}

static void
bt_default_address(uint8_t out[6])
{
	static const uint8_t fallback[6] = { 0x02, 0x41, 0x4e, 0x4e, 0x00, 0x02 };
	IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	OSData * data = chosen != NULL ? OSDynamicCast(OSData, chosen->getProperty("mac-address-bluetooth0")) : NULL;

	if (data != NULL && data->getLength() >= 6) {
		memcpy(out, data->getBytesNoCopy(), 6);
	} else {
		memcpy(out, fallback, 6);
	}
	OSSafeReleaseNULL(chosen);
}

static IOService *
bt_publish(IOService * service, IOService * provider, const char * name)
{
	if (service == NULL || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	(void)service->attachToParent(provider, gIODTPlane);
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	return service;
}

void
AnnibaleBTStart(const char * bsdname, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	AnnibaleBT * bt;
	OSString * s;
	uint8_t initial[6];

	if (gAnnibaleDisk == NULL) {
		AnnibaleDiskStart(0);
	}
	if (gAnnibaleBT != NULL) {
		if (address != NULL) {
			AnnibaleBTSetAddress(address);
		}
		return;
	}
	bt = OSTypeAlloc(AnnibaleBT);
	if (platform == NULL || bt == NULL || !bt->init()) {
		OSSafeReleaseNULL(bt);
		IOLog("AnnibaleBT: cannot create the service\n");
		return;
	}
	if ((s = OSString::withCString(bsdname)) != NULL) {
		bt->setProperty("BSD Name", s);
		s->release();
	}
	if ((s = OSString::withCString("uart")) != NULL) {
		bt->setProperty("transport", s);
		bt->setProperty("BluetoothTransport", s);
		s->release();
	}
	if (address != NULL) {
		memcpy(initial, address, 6);
	} else {
		bt_default_address(initial);
	}
	bt_set_address(bt, initial);
	bt->setProperty("IOBuiltin", true);
	bt->setProperty("Built-In", true);
	bt->setProperty("bluetooth-active", true);
	{
		OSNumber * pwr = OSNumber::withNumber(1u, 32);
		if (pwr != NULL) {
			bt->setProperty("DevicePowerState", pwr);
			bt->setProperty("PowerState", pwr);
			pwr->release();
		}
	}
	if (bt_publish(bt, platform, "bluetooth") == NULL) {
		IOLog("AnnibaleBT: the service did not start\n");
	} else {
		IOLog("AnnibaleBT: published IOKit service 'bluetooth' (BSD Name %s, address %02x:%02x:%02x:%02x:%02x:%02x)\n",
		    bsdname, initial[0], initial[1], initial[2], initial[3], initial[4], initial[5]);
		gAnnibaleBT = bt;
	}
}

void
AnnibaleBTSetAddress(const uint8_t * address)
{
	if (gAnnibaleBT == NULL || address == NULL) {
		return;
	}
	bt_set_address(gAnnibaleBT, address);
	IOLog("AnnibaleBT: local-mac-address %02x:%02x:%02x:%02x:%02x:%02x\n", address[0], address[1], address[2],
	    address[3], address[4], address[5]);
}

void
AnnibaleBTSetPowerState(bool on)
{
	if (gAnnibaleBT == NULL) {
		return;
	}
	OSNumber * n = OSNumber::withNumber(on ? 1u : 0u, 32);
	if (n != NULL) {
		gAnnibaleBT->setProperty("DevicePowerState", n);
		gAnnibaleBT->setProperty("PowerState", n);
		n->release();
	}
	gAnnibaleBT->setProperty("bluetooth-active", on ? kOSBooleanTrue : kOSBooleanFalse);
	IOLog("AnnibaleBT: power state updated to %s (DevicePowerState=%u)\n", on ? "ON" : "OFF", on ? 1u : 0u);
}

static AppleAPFSVolume *
publish_volume(AppleAPFSContainer * container, const char * bsdname, const char * volname,
    const char * role, uint32_t roleMask, const char * uuid, uint64_t sz, bool writable)
{
	AppleAPFSVolume * vol = OSTypeAlloc(AppleAPFSVolume);
	OSString * s;
	OSNumber * n;
	OSArray * arr;

	if (container == NULL || vol == NULL || !vol->init()) {
		OSSafeReleaseNULL(vol);
		return NULL;
	}
	if ((s = OSString::withCString(bsdname)) != NULL) {
		vol->setProperty("BSD Name", s);
		s->release();
	}
	if ((s = OSString::withCString(volname)) != NULL) {
		vol->setProperty("Volume Name", s);
		s->release();
	}
	if ((s = OSString::withCString(uuid)) != NULL) {
		vol->setProperty("VolumeUUID", s);
		vol->setProperty("UUID", s);
		s->release();
	}
	if ((s = OSString::withCString("Apple_APFS_Volume")) != NULL) {
		vol->setProperty("Content", s);
		vol->setProperty("Content Hint", s);
		s->release();
	}
	if ((s = OSString::withCString(role)) != NULL) {
		vol->setProperty("Role Description", s);
		vol->setProperty("Role", s);
		s->release();
	}
	if ((arr = OSArray::withCapacity(1)) != NULL) {
		if ((s = OSString::withCString(role)) != NULL) {
			arr->setObject(s);
			s->release();
		}
		vol->setProperty("Roles", arr);
		arr->release();
	}
	if ((n = OSNumber::withNumber(sz, 64)) != NULL) {
		vol->setProperty("Size", n);
		n->release();
	}
	if ((n = OSNumber::withNumber(4096ULL, 32)) != NULL) {
		vol->setProperty("Preferred Block Size", n);
		vol->setProperty("Block Size", n);
		n->release();
	}
	if ((n = OSNumber::withNumber((uint64_t)roleMask, 32)) != NULL) {
		vol->setProperty("APFSRole", n);
		n->release();
	}
	if ((s = OSString::withCString("apfs-2236.101.1")) != NULL) {
		vol->setProperty("Formatted By", s);
		s->release();
	}
	vol->setProperty("Whole", false);
	vol->setProperty("Writable", writable);
	vol->setProperty("Internal", true);
	vol->setProperty("IOBuiltin", true);
	vol->setProperty("Leaf", true);
	vol->setProperty("Open", true);

	if (bt_publish(vol, container, bsdname) != NULL) {
		IOLog("AnnibaleDisk: published volume '%s' (%s, Role %s)\n", bsdname, volname, role);
		return vol;
	}
	return NULL;
}

void
AnnibaleDiskStart(uint64_t size_bytes)
{
	IOService * platform = IOService::getPlatform();
	AnnibaleDisk * disk0;
	AnnibaleDisk * disk0s1;
	AppleAPFSContainer * container;
	OSString * s;
	OSNumber * n;
	uint64_t sz = size_bytes != 0 ? size_bytes : 128000000000ULL;

	if (gAnnibaleDisk != NULL) {
		if ((n = OSNumber::withNumber(sz, 64)) != NULL) {
			gAnnibaleDisk->setProperty("Size", n);
			n->release();
		}
		return;
	}

	/* 1. disk0: physical storage device */
	disk0 = OSTypeAlloc(AnnibaleDisk);
	if (platform == NULL || disk0 == NULL || !disk0->init()) {
		OSSafeReleaseNULL(disk0);
		return;
	}
	if ((s = OSString::withCString("disk0")) != NULL) {
		disk0->setProperty("BSD Name", s);
		s->release();
	}
	if ((s = OSString::withCString("GUID_partition_scheme")) != NULL) {
		disk0->setProperty("Content", s);
		disk0->setProperty("Content Hint", s);
		s->release();
	}
	if ((n = OSNumber::withNumber(sz, 64)) != NULL) {
		disk0->setProperty("Size", n);
		n->release();
	}
	if ((n = OSNumber::withNumber(4096ULL, 32)) != NULL) {
		disk0->setProperty("Preferred Block Size", n);
		disk0->setProperty("Block Size", n);
		n->release();
	}
	disk0->setProperty("Whole", true);
	disk0->setProperty("Writable", true);
	disk0->setProperty("Internal", true);
	disk0->setProperty("IOBuiltin", true);
	disk0->setProperty("Leaf", false);
	if (bt_publish(disk0, platform, "disk0") == NULL) {
		return;
	}
	IOLog("AnnibaleDisk: published IOKit service 'disk0' (Size %llu bytes)\n", sz);
	gAnnibaleDisk = disk0;

	/* 2. disk0s1: Apple_APFS container partition */
	disk0s1 = OSTypeAlloc(AnnibaleDisk);
	if (disk0s1 != NULL && disk0s1->init()) {
		if ((s = OSString::withCString("disk0s1")) != NULL) {
			disk0s1->setProperty("BSD Name", s);
			s->release();
		}
		if ((s = OSString::withCString("Apple_APFS")) != NULL) {
			disk0s1->setProperty("Content", s);
			disk0s1->setProperty("Content Hint", s);
			s->release();
		}
		if ((n = OSNumber::withNumber(sz, 64)) != NULL) {
			disk0s1->setProperty("Size", n);
			n->release();
		}
		if ((n = OSNumber::withNumber(4096ULL, 32)) != NULL) {
			disk0s1->setProperty("Preferred Block Size", n);
			disk0s1->setProperty("Block Size", n);
			n->release();
		}
		if ((n = OSNumber::withNumber(1ULL, 32)) != NULL) {
			disk0s1->setProperty("Partition ID", n);
			n->release();
		}
		disk0s1->setProperty("Whole", false);
		disk0s1->setProperty("Writable", true);
		disk0s1->setProperty("Internal", true);
		disk0s1->setProperty("IOBuiltin", true);
		disk0s1->setProperty("Leaf", false);
		(void)bt_publish(disk0s1, disk0, "disk0s1");
	}

	/* 3. disk1: AppleAPFSContainer */
	container = OSTypeAlloc(AppleAPFSContainer);
	if (container != NULL && container->init()) {
		IOService * parent = (disk0s1 != NULL) ? (IOService *)disk0s1 : (IOService *)disk0;
		if ((s = OSString::withCString("disk1")) != NULL) {
			container->setProperty("BSD Name", s);
			s->release();
		}
		if ((s = OSString::withCString("Apple_APFS_Container")) != NULL) {
			container->setProperty("Content", s);
			container->setProperty("Content Hint", s);
			s->release();
		}
		if ((s = OSString::withCString("disk0s1")) != NULL) {
			container->setProperty("Designated Physical Store", s);
			s->release();
		}
		if ((s = OSString::withCString("494F5332-3643-4F4E-5441-494E45520000")) != NULL) {
			container->setProperty("ContainerUUID", s);
			container->setProperty("UUID", s);
			s->release();
		}
		if ((n = OSNumber::withNumber(sz, 64)) != NULL) {
			container->setProperty("Size", n);
			n->release();
		}
		if ((n = OSNumber::withNumber(4096ULL, 32)) != NULL) {
			container->setProperty("Preferred Block Size", n);
			container->setProperty("Block Size", n);
			n->release();
		}
		container->setProperty("Whole", true);
		container->setProperty("Writable", true);
		container->setProperty("Internal", true);
		container->setProperty("IOBuiltin", true);
		container->setProperty("Leaf", false);
		container->setProperty("Fusion", false);
		container->setProperty("Encrypted", false);

		if (bt_publish(container, parent, "AppleAPFSContainer") != NULL) {
			IOLog("AnnibaleDisk: published AppleAPFSContainer 'disk1'\n");

			/* 4. Volumes: System, Data, Preboot */
			publish_volume(container, "disk1s1", "System", "System", 0x1,
			    "494F5332-3653-5953-5445-4D564F4C0001", sz, false);
			publish_volume(container, "disk1s2", "Data", "Data", 0x40,
			    "494F5332-3644-4154-4156-4F4C554D4502", sz, true);
			publish_volume(container, "disk1s3", "Preboot", "Preboot", 0x10,
			    "494F5332-3650-5245-424F-4F54564F4C03", sz, true);
		}
	}
}

/*
 * RedwoodBT: the IOKit service by which iOS's bluetoothd finds the Bluetooth UART.
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
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

extern "C" void RedwoodBTStart(const char * bsdname, const uint8_t * address);
extern "C" void RedwoodBTSetAddress(const uint8_t * address);

class RedwoodBT : public IOService
{
	OSDeclareDefaultStructors(RedwoodBT);
};

OSDefineMetaClassAndStructors(RedwoodBT, IOService);

static RedwoodBT * gRedwoodBT;

static void
bt_set_address(RedwoodBT * bt, const uint8_t * address)
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
	static const uint8_t fallback[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
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
RedwoodBTStart(const char * bsdname, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	RedwoodBT * bt;
	OSString * s;
	uint8_t initial[6];

	if (gRedwoodBT != NULL) {
		if (address != NULL) {
			RedwoodBTSetAddress(address);
		}
		return;
	}
	bt = OSTypeAlloc(RedwoodBT);
	if (platform == NULL || bt == NULL || !bt->init()) {
		OSSafeReleaseNULL(bt);
		IOLog("RedwoodBT: cannot create the service\n");
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
	if (bt_publish(bt, platform, "bluetooth") == NULL) {
		IOLog("RedwoodBT: the service did not start\n");
	} else {
		IOLog("RedwoodBT: published IOKit service 'bluetooth' (BSD Name %s, address %02x:%02x:%02x:%02x:%02x:%02x)\n",
		    bsdname, initial[0], initial[1], initial[2], initial[3], initial[4], initial[5]);
		gRedwoodBT = bt;
	}
}

void
RedwoodBTSetAddress(const uint8_t * address)
{
	if (gRedwoodBT == NULL || address == NULL) {
		return;
	}
	bt_set_address(gRedwoodBT, address);
	IOLog("RedwoodBT: local-mac-address %02x:%02x:%02x:%02x:%02x:%02x\n", address[0], address[1], address[2],
	    address[3], address[4], address[5]);
}

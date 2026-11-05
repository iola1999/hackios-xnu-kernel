/*
 * Stand-ins for Apple's closed kernel extensions: just enough of each for the stock iOS
 * user space to work on a machine without Apple hardware. The platform expert
 * (GenericARMPE) starts them once the registry is up.
 *
 *   MiniIOSurface.cpp              IOSurface: pixel buffers shared between processes
 *   MiniIOMobileFramebuffer.cpp    IOMobileFramebuffer: the display (the boot framebuffer)
 *   MiniAppleARMBacklight.cpp      AppleARMBacklight: the display's brightness, as CoreBrightness sets it
 *   MiniAppleKeyStore.cpp          AppleKeyStore: the keybag of a device without a passcode
 *   MiniIONetworkStack.cpp         IONetworkStack: what configd's interface namer waits for
 */
#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H

#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void MiniAppleARMBacklightStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniIONetworkStackStart(void);
extern "C" void RedwoodBTStart(const char * bsdname, const uint8_t * address);
extern "C" void RedwoodBTSetAddress(const uint8_t * address);

/* A 32-bit number into a property dictionary (IOMobileFramebuffer's modes, the backlight's parameters). */
static inline void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * Between the framebuffer and the backlight: IOMFBBrightnessLevel handed on, the picture darkened (fb_dim),
 * the panel's own brightness set over DSI (fb_dcs).
 */
IOReturn MiniAppleARMBacklightFramebufferProperties(OSObject * properties);
void MiniIOMobileFramebufferDim(uint32_t level);
int MiniIOMobileFramebufferDimShown(void);
bool MiniIOMobileFramebufferDCS(uint32_t value);

#endif /* _IOKIT_MINIDRIVERS_H */

/* A minimal "AppleSMC" service for a platform with no System Management Controller.
 *
 * iOS daemons take the AppleSMC service for granted: batteryintelligenced reads its
 * key table through the user client and crashes building a dictionary when the
 * lookup misses (IOServiceGetMatchingService(IOServiceMatching("AppleSMC")) returns
 * 0, its smcKeys property stays nil, and @{@"smcData": nil} is an
 * NSInvalidArgumentException -- 37 useraborts in one boot of
 * kernel-12377-say16-ubo15). IOServiceMatching is a class match, so the service
 * class below is named AppleSMC. Its user client answers every key query with
 * kSMCKeyNotFound: an empty but valid key table. Callers that handle a missing
 * SMC key then see the same result as on hardware whose SMC lacks that key.
 *
 * The status byte lives at offset 0x28 of the 0xA8-byte SMCParamStruct this
 * protocol uses; 0x84 is kSMCKeyNotFound. Method selectors follow AppleSMCFamily's
 * user client: 0 is the open handshake, 1 is close, 2 is the struct call that
 * carries every key op (read, write, index, info -- all answered "not found").
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSByteOrder.h>

#define ANN_SMC_STRUCT_SIZE  0xA8
#define ANN_SMC_STATUS_OFF   0x28
#define ANN_SMC_KEY_NOTFOUND 0x84

class AppleSMCUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleSMCUserClient);
public:
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleSMCUserClient, IOUserClient);

IOReturn
AppleSMCUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	(void)dispatch; (void)target; (void)reference;
	switch (selector) {
	case 0: /* kSMCUserClientOpen */
	case 1: /* kSMCUserClientClose */
		return kIOReturnSuccess;
	case 2: /* kSMCHandleYPCEvent: the 0xA8-byte SMCParamStruct call */
		if (args->structureInput == NULL || args->structureOutput == NULL ||
		    args->structureInputSize != ANN_SMC_STRUCT_SIZE ||
		    args->structureOutputSize < ANN_SMC_STRUCT_SIZE) {
			return kIOReturnBadArgument;
		}
		memcpy(args->structureOutput, args->structureInput, ANN_SMC_STRUCT_SIZE);
		((uint8_t *)args->structureOutput)[ANN_SMC_STATUS_OFF] = ANN_SMC_KEY_NOTFOUND;
		args->structureOutputSize = ANN_SMC_STRUCT_SIZE;
		return kIOReturnSuccess;
	default:
		return kIOReturnUnsupported;
	}
}

class AppleSMC : public IOService
{
	OSDeclareDefaultStructors(AppleSMC);
public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleSMC, IOService);

bool
AppleSMC::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	if (!setProperty(kIOUserClientClassKey, "AppleSMCUserClient")) {
		return false;
	}
	setName("AppleSMC");
	registerService(kIOServiceSynchronous);
	return true;
}

extern "C" bool AnnibaleSMCStart(void);

bool
AnnibaleSMCStart(void)
{
	IOPlatformExpert * provider = IOService::getPlatform();
	if (provider == NULL) {
		return false;
	}
	AppleSMC * smc = new AppleSMC;
	if (smc == NULL || !smc->init() || !smc->attach(provider) || !smc->start(provider)) {
		if (smc != NULL) {
			smc->release();
		}
		return false;
	}
	return true;
}

/*
 * AnnibaleNVRAM: where IODTNVRAM keeps its banks, the start of the darwin_nvram partition
 * (bsd/dev/annibale/annibale_nvram.inc), as AppleARMNORNVRAM keeps them in an iPhone's NOR flash.
 *
 * The loader reads the banks at boot and gives IODTNVRAM the newest whole one as nvram-proxy-data,
 * with nvram-bank-size, nvram-bank-count and nvram-current-bank in /chosen
 * (xnuloader/loader_04_fault_payload_nvram.inc). On a sync IONVRAMCHRPHandler writes the next bank
 * in turn: select(), eraseBank(), write(0, the image, the bank size), sync(). Syncs come from
 * IONVRAM-SYNCNOW-PROPERTY, PEWriteNVRAMProperty(), PESyncNVRAM() at restart and shutdown
 * (IOPMrootDomain) and at the end of a run (annibale_var_save_on_return()).
 *
 * A bank's first block reaches the flash last, after the rest of it. Until then the bank keeps its
 * old header and generation, and both the loader (which checks the Adler-32 of the whole bank) and
 * IONVRAMCHRPHandler::findCurrentBank() (which reads only the header) take the other bank.
 *
 * The controller is registered only when /chosen has more than one bank, which the loader gives
 * when it has read them from darwin_nvram. A loader without darwin_nvram support gives one bank and
 * an empty image; writing that image back would replace what the partition keeps.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/nvram/IONVRAMController.h>
#include <libkern/c++/OSData.h>

extern "C" {
uint64_t annibale_nvram_bytes(void);
int annibale_nvram_io(bool write, uint64_t offset, void *buf, uint64_t len);
int annibale_nvram_flush(void);
void AnnibaleNVRAMStart(void);
bool annibale_nvram_kept(void);
}

#define kNVRAMHeadBytes         4096u   /* the block a bank's header is in: written last */
#define kNVRAMGenerationAt      20u     /* struct apple_nvram_header (IONVRAMCHRPHandler.cpp) */

class AnnibaleNVRAM : public IONVRAMController
{
	OSDeclareDefaultStructors(AnnibaleNVRAM);

public:
	uint32_t bankSize, bankCount, bank;

	virtual IOReturn select(uint32_t which) APPLE_KEXT_OVERRIDE;
	virtual IOReturn eraseBank(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn read(IOByteCount offset, UInt8 *buffer, IOByteCount length) APPLE_KEXT_OVERRIDE;
	virtual IOReturn write(IOByteCount offset, UInt8 *buffer, IOByteCount length) APPLE_KEXT_OVERRIDE;
	virtual void sync(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AnnibaleNVRAM, IONVRAMController);

static AnnibaleNVRAM *gAnnibaleNVRAM;

IOReturn
AnnibaleNVRAM::select(uint32_t which)
{
	if (which >= bankCount) {
		return kIOReturnBadArgument;
	}
	bank = which;
	return kIOReturnSuccess;
}

/* UFS rewrites blocks in place: there is nothing to erase. */
IOReturn
AnnibaleNVRAM::eraseBank(void)
{
	return kIOReturnSuccess;
}

IOReturn
AnnibaleNVRAM::read(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
	if (offset > bankSize || length > bankSize - offset) {
		return kIOReturnBadArgument;
	}
	return annibale_nvram_io(false, (uint64_t)bank * bankSize + offset, buffer, length) == 0 ?
	       kIOReturnSuccess : kIOReturnIOError;
}

IOReturn
AnnibaleNVRAM::write(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
	uint64_t at = (uint64_t)bank * bankSize + offset;
	uint32_t generation = 0;
	int error;

	if (offset > bankSize || length > bankSize - offset) {
		return kIOReturnBadArgument;
	}
	if (offset == 0 && length > kNVRAMHeadBytes) {
		error = annibale_nvram_io(true, at + kNVRAMHeadBytes, buffer + kNVRAMHeadBytes, length - kNVRAMHeadBytes);
		if (error == 0) {
			error = annibale_nvram_flush();
		}
		if (error == 0) {
			error = annibale_nvram_io(true, at, buffer, kNVRAMHeadBytes);
		}
		memcpy(&generation, buffer + kNVRAMGenerationAt, sizeof(generation));
	} else {
		error = annibale_nvram_io(true, at, buffer, length);
	}
	if (error != 0) {
		IOLog("AnnibaleNVRAM: bank %u: the write failed (%d)\n", bank, error);
		return kIOReturnIOError;
	}
	IOLog("AnnibaleNVRAM: bank %u written, generation %u\n", bank, generation);
	return kIOReturnSuccess;
}

void
AnnibaleNVRAM::sync(void)
{
	int error = annibale_nvram_flush();

	if (error != 0) {
		IOLog("AnnibaleNVRAM: bank %u: the flush failed (%d)\n", bank, error);
	}
}

static uint32_t
chosen_u32(IORegistryEntry *chosen, const char *name)
{
	OSData *data = OSDynamicCast(OSData, chosen->getProperty(name));
	uint32_t value = 0;

	if (data != NULL && data->getLength() == sizeof(value)) {
		memcpy(&value, data->getBytesNoCopy(), sizeof(value));
	}
	return value;
}

/*
 * From annibale_ufs_init(), in bsd_init: the platform expert has started by then (GenericARMPE::start
 * returns before the CPU count bsd_init waits for is known), so IODTNVRAM is there to take the
 * controller.
 */
void
AnnibaleNVRAMStart(void)
{
	IORegistryEntry *chosen;
	IOService *platform = IOService::getPlatform();
	uint64_t room = annibale_nvram_bytes();
	uint32_t size = 0, count = 0;
	AnnibaleNVRAM *nvram;

	if (room == 0 || gAnnibaleNVRAM != NULL || platform == NULL) {
		return;
	}
	chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (chosen != NULL) {
		size = chosen_u32(chosen, "nvram-bank-size");
		count = chosen_u32(chosen, "nvram-bank-count");
		chosen->release();
	}
	if (count < 2 || size < 2 * kNVRAMHeadBytes || size % kNVRAMHeadBytes != 0 || (uint64_t)size * count > room) {
		IOLog("AnnibaleNVRAM: the loader gives %u bank(s) of %u bytes: NVRAM is not written to darwin_nvram\n",
		    count, size);
		return;
	}
	nvram = OSTypeAlloc(AnnibaleNVRAM);
	if (nvram == NULL || !nvram->init() || !nvram->attach(platform)) {
		OSSafeReleaseNULL(nvram);
		IOLog("AnnibaleNVRAM: cannot create the controller\n");
		return;
	}
	nvram->bankSize = size;
	nvram->bankCount = count;
	gAnnibaleNVRAM = nvram;
	IOLog("AnnibaleNVRAM: %u banks of %u bytes at the start of darwin_nvram\n", count, size);
	nvram->registerService();       /* IONVRAMController: the platform expert hands it to IODTNVRAM */
}

/* Whether NVRAM is written to darwin_nvram, and so lasts across restarts. */
bool
annibale_nvram_kept(void)
{
	return gAnnibaleNVRAM != NULL;
}

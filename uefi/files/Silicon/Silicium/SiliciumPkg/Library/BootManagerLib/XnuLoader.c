/**
  XNU payload entry point for annibale.

  Runs when BDS finds nothing to boot. The boot shim has copied the payload
  (see XnuLoader.h) to a fixed reserved address; if it carries "loader.efi",
  the image is started straight from memory so the loader can be iterated on
  without rebuilding this firmware. XnuLoaderMain does not return: a missing,
  invalid or returned-from payload ends in a PSCI SYSTEM_RESET, with no
  console, storage or firmware call before it, so the device boots again
  without a key press.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/ArmMonitorLib.h>
#include <IndustryStandard/ArmStdSmc.h>

#include "XnuLoader.h"

// XNU-TERM-BEGIN -- tools/test-xnu-payload-terminal.py replaces exactly this
// block on the host; on the target the block below runs as written.
__attribute__((noreturn))
STATIC
VOID
EFIAPI
XnuTerminalReset (
  VOID
  )
{
  ARM_MONITOR_ARGS  Args = { 0 };

  //
  // PSCI SYSTEM_RESET straight to the secure monitor, the call the resident
  // loader's own reset makes. The firmware's reset service runs the reset
  // notification callbacks first: on annibale (2026-11-05) that path brought
  // the phone back after 5 min 26 s, this call after 34 s. Nothing is written
  // to storage before the reset: the loader image is on a boot partition
  // (boot_a), and a log written there would damage the image the next boot
  // reads.
  // SYSTEM_RESET does not return when the reset is taken; if it does, this CPU
  // stops with interrupts masked and only a long press of the power key brings
  // the device back.
  //
  Args.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
  ArmMonitorCall (&Args);

  __asm__ volatile ("msr daifset, #0xf" ::: "memory");

  for (;;) {
  }
}
// XNU-TERM-END

//
// The payload is a list of named blobs. If it carries "loader.efi", that is
// the real XNU loader: start it straight from memory, so the loader can be
// iterated on without rebuilding this firmware.
//
STATIC
VOID
StartPayloadLoader ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;
  UINTN              PaddedSize;

  //
  // Header->Size declares the body length only: the container including this
  // 8-byte header may span at most XNU_PAYLOAD_MAX_SIZE of the reserved
  // window, so the cap subtracts the header and End below stays inside the
  // window, below the panic-log tail. Every later bound derives from Size by
  // subtraction (remaining minus entry header) instead of pointer addition,
  // which could overflow on a bogus size. This function can only bound what
  // the container declares: that the shim really wrote the declared bytes
  // inside the reserved window, and that the declared body matches the real
  // decompressed source length, are premises checked offline by the packer
  // and by the shim's own copy cap, not here.
  //
  if ((Header->Magic != XNU_PAYLOAD_MAGIC) || (Header->Size > XNU_PAYLOAD_MAX_SIZE - sizeof (XNU_PAYLOAD_HEADER))) {
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while ((UINTN)(End - Cursor) >= sizeof (XNU_PAYLOAD_ENTRY)) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    PaddedSize = ALIGN_VALUE ((UINTN)Entry->Size, 16);

    if (PaddedSize > ((UINTN)(End - Cursor) - sizeof (XNU_PAYLOAD_ENTRY))) {
      //
      // The entry claims data past the end of the container: an invalid
      // payload, so no address goes to LoadImage.
      //
      return;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_HANDLE Handle;

      if (Entry->Size == 0) {
        return;
      }

      if (!EFI_ERROR (gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle))) {
        gBS->StartImage (Handle, NULL, NULL);
      }

      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + PaddedSize;
  }
}

VOID
XnuLoaderMain ()
{
  StartPayloadLoader ();

  //
  // Reached when no payload image is started and when a started image
  // returned -- StartImage returning EFI_SUCCESS is included, because a
  // payload that keeps running never comes back here.
  //
  XnuTerminalReset ();
}

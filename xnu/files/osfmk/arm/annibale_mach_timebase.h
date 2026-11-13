#ifndef _ARM_ANNIBALE_MACH_TIMEBASE_H_
#define _ARM_ANNIBALE_MACH_TIMEBASE_H_

#include <stdbool.h>
#include <stdint.h>
#include <machine/config.h>
#include <pexpert/pexpert.h>

#define ANNIBALE_HW_TIMEBASE_HZ   19200000
#define ANNIBALE_MACH_TIMEBASE_HZ 24000000

static inline bool
annibale_mach_timebase_scaled(void)
{
#if defined(ANNIBALE_ARM64E_USERLAND)
	return gPEClockFrequencyInfo.dec_clock_rate_hz == ANNIBALE_HW_TIMEBASE_HZ &&
	       gPEClockFrequencyInfo.timebase_frequency_hz == ANNIBALE_MACH_TIMEBASE_HZ;
#else
	return false;
#endif
}

/* AudioSession uses 24 MHz host ticks; the hardware timer remains at 19.2 MHz. */
static inline uint64_t
annibale_hw_ticks_to_mach(uint64_t hardware_ticks)
{
	return annibale_mach_timebase_scaled() ? hardware_ticks + (hardware_ticks >> 2) : hardware_ticks;
}

/* Round up so a hardware expiration does not precede the mach deadline. */
static inline uint32_t
annibale_mach_interval_to_hw(uint32_t mach_interval)
{
	return annibale_mach_timebase_scaled() ? (uint32_t)(((uint64_t)mach_interval * 4 + 4) / 5) : mach_interval;
}

#endif

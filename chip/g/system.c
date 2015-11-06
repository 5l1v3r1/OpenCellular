/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cpu.h"
#include "registers.h"
#include "system.h"
#include "task.h"

static void check_reset_cause(void)
{
	uint32_t reset_source = GR_PMU_RSTSRC;
	uint32_t flags = 0;

	/* Clear the reset source now we have recorded it */
	GR_PMU_CLRRST = 1;

	if (reset_source & (1 << GC_PMU_RSTSRC_POR_LSB))
		flags |= RESET_FLAG_POWER_ON;
	else if (reset_source & (1 << GC_PMU_RSTSRC_EXIT_LSB))
		flags |= RESET_FLAG_WAKE_PIN;

	if (reset_source & (1 << GC_PMU_RSTSRC_WDOG_LSB))
		flags |= RESET_FLAG_WATCHDOG;

	if (reset_source & (1 << GC_PMU_RSTSRC_SOFTWARE_LSB))
		flags |= RESET_FLAG_HARD;
	if (reset_source & (1 << GC_PMU_RSTSRC_SYSRESET_LSB))
		flags |= RESET_FLAG_SOFT;

	if (reset_source & (1 << GC_PMU_RSTSRC_FST_BRNOUT_LSB))
		flags |= RESET_FLAG_BROWNOUT;

	if (reset_source && !flags)
		flags |= RESET_FLAG_OTHER;

	system_set_reset_flags(flags);
}

void system_pre_init(void)
{
	check_reset_cause();
}

void system_reset(int flags)
{
	/* Disable interrupts to avoid task swaps during reboot */
	interrupt_disable();

	if (flags & SYSTEM_RESET_HARD) {
		/* Reset the full microcontroller */
		GR_PMU_GLOBAL_RESET = GC_PMU_GLOBAL_RESET_KEY;
	} else {
		/* Soft reset is also fairly hard, and requires
		 * permission registers to be reset to their initial
		 * state.  To accomplish this, first register a wakeup
		 * timer and then enter lower power mode. */

		/* Low speed timers continue to run in low power mode. */
		GREG32(TIMELS, TIMER1_CONTROL) = 0x1;
		/* Wait for this long. */
		GREG32(TIMELS, TIMER1_LOAD) = 1;
		/* Setup wake-up on Timer1 firing. */
		GREG32(PMU, EXITPD_MASK) =
			GC_PMU_EXITPD_MASK_TIMELS0_PD_EXIT_TIMER1_MASK;

		/* All the components to power cycle. */
		GREG32(PMU, LOW_POWER_DIS) =
			GC_PMU_LOW_POWER_DIS_VDDL_MASK |
			GC_PMU_LOW_POWER_DIS_VDDIOF_MASK |
			GC_PMU_LOW_POWER_DIS_VDDXO_MASK |
			GC_PMU_LOW_POWER_DIS_JTR_RC_MASK;
		/* Start low power sequence. */
		REG_WRITE_MLV(GREG32(PMU, LOW_POWER_DIS),
			GC_PMU_LOW_POWER_DIS_START_MASK,
			GC_PMU_LOW_POWER_DIS_START_LSB,
			1);
	}

	/* Wait for reboot; should never return  */
	asm("wfi");
}

const char *system_get_chip_vendor(void)
{
	return "g";
}

const char *system_get_chip_name(void)
{
	return "cr50";
}

const char *system_get_chip_revision(void)
{
	int build_date = GR_SWDP_BUILD_DATE;
	int build_time = GR_SWDP_BUILD_TIME;

	if ((build_date != GC_SWDP_BUILD_DATE_DEFAULT) ||
	    (build_time != GC_SWDP_BUILD_TIME_DEFAULT))
		return GC_REVISION_STR" BUILD MISMATCH!";
	else
		return GC_REVISION_STR;
}

/* TODO(crosbug.com/p/33822): Where can we store stuff persistently? */
int system_get_vbnvcontext(uint8_t *block)
{
	return 0;
}

int system_set_vbnvcontext(const uint8_t *block)
{
	return 0;
}

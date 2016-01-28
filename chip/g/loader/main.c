/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "debug_printf.h"
#include "printf.h"
#include "registers.h"
#include "setup.h"
#include "signed_header.h"
#include "system.h"
#include "trng.h"
#include "uart.h"

/*
 * This file is a proof of concept stub which will be extended and split into
 * appropriate pieces sortly, when full blown support for cr50 bootrom is
 * introduced.
 */
uint32_t sleep_mask;

timestamp_t get_time(void)
{
	timestamp_t ret;

	ret.val = 0;

	return ret;
}

static int panic_txchar(void *context, int c)
{
	if (c == '\n')
		panic_txchar(context, '\r');

	/* Wait for space in transmit FIFO */
	while (!uart_tx_ready())
		;

	/* Write the character directly to the transmit FIFO */
	uart_write_char(c);

	return 0;
}

void panic_puts(const char *outstr)
{
	/* Put all characters in the output buffer */
	while (*outstr)
		panic_txchar(NULL, *outstr++);
}

void panic_printf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfnprintf(panic_txchar, NULL, format, args);
	va_end(args);
}

/* Returns 1 if version a is newer, 0 otherwise. */
int is_newer_than(const struct SignedHeader *a, const struct SignedHeader *b)
{
	if (a->epoch_ > b->epoch_)
		return 1;
	if (a->epoch_ < b->epoch_)
		return 0;
	if (a->major_ > b->major_)
		return 1;
	if (a->major_ < b->major_)
		return 0;
	if (a->minor_ > b->minor_)
		return 1;
	if (a->minor_ < b->minor_)
		return 0;
	return 0;
}

int main(void)
{
	const struct SignedHeader *a, *b, *first, *second;
	init_trng();
	uart_init();
	debug_printf("\n\n%s bootloader, %8u_%u@%u, %sUSB, %s crypto\n",
		     STRINGIFY(BOARD), GREG32(SWDP, BUILD_DATE),
		     GREG32(SWDP, BUILD_TIME), GREG32(SWDP, P4_LAST_SYNC),
		     (GREG32(SWDP, FPGA_CONFIG) &
		      GC_CONST_SWDP_FPGA_CONFIG_USB_8X8CRYPTO) ? "" : "no ",
		     (GREG32(SWDP, FPGA_CONFIG) &
		      GC_CONST_SWDP_FPGA_CONFIG_NOUSB_CRYPTO) ? "full" : "8x8");
	unlockFlashForRW();

	a = (const struct SignedHeader *)(CONFIG_PROGRAM_MEMORY_BASE +
					  CONFIG_RW_MEM_OFF);
	b = (const struct SignedHeader *)(CONFIG_PROGRAM_MEMORY_BASE +
					  CONFIG_RW_B_MEM_OFF);
	/* Default to loading the older version first.
	 * Run from bank a if the versions are equal.
	 */
	if (is_newer_than(a, b)) {
		first = b;
		second = a;
	} else {
		first = a;
		second = b;
	}
	if (GREG32(PMU, PWRDN_SCRATCH30) == 0xcafebabe) {
		/* Launch from the alternate bank first.
		 * This knob will be used to attempt to load the newer version
		 * after an update and to run from bank b in the face of flash
		 * integrity issues.
		 */
		debug_printf("PWRDN_SCRATCH30 set to magic value\n");
		GREG32(PMU, PWRDN_SCRATCH30) = 0x0;
		a = first;
		first = second;
		second = a;
	}
	tryLaunch((uint32_t)first, CONFIG_FLASH_SIZE/2 - CONFIG_RW_MEM_OFF);
	debug_printf("Failed to launch.\n");
	debug_printf("Attempting to load the alternate image.\n");
	tryLaunch((uint32_t)second, CONFIG_FLASH_SIZE/2 - CONFIG_RW_MEM_OFF);
	debug_printf("No valid image found, not sure what to do...\n");
	/* TODO: Some applications might want to reboot instead. */
	halt();
	return 1;
}

void panic_reboot(void)
{
	panic_puts("\n\nRebooting...\n");
	system_reset(0);
}

void interrupt_disable(void)
{
	asm("cpsid i");
}

static int printchar(void *context, int c)
{
	if (c == '\n')
		uart_write_char('\r');
	uart_write_char(c);

	return 0;
}

void debug_printf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfnprintf(printchar, NULL, format, args);
	va_end(args);
}

/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_CONFIG_CHIP_H
#define __CROS_EC_CONFIG_CHIP_H

/* CPU core BFD configuration */
#include "core/cortex-m/config_core.h"

/* 32k hz internal oscillator frequency (FRCLK) */
#define INT_32K_CLOCK 32768

/* Number of IRQ vectors on the NVIC */
#define CONFIG_IRQ_COUNT 64

/* Use a bigger console output buffer */
#undef CONFIG_UART_TX_BUF_SIZE
#define CONFIG_UART_TX_BUF_SIZE 8192

/*
 * Interval between HOOK_TICK notifications
 * Notice instant wake-up from deep-idle cannot exceed 200 ms
 */
#define HOOK_TICK_INTERVAL_MS 200
#define HOOK_TICK_INTERVAL    (HOOK_TICK_INTERVAL_MS * MSEC)

/* Maximum number of deferrable functions */
#define DEFERRABLE_MAX_COUNT 8

/*
 * Number of I2C controllers. Controller 0 has 2 ports, so the chip has one
 * additional port.
 */
#define CONFIG_I2C_MULTI_PORT_CONTROLLER
/* Number of I2C controllers */
#define I2C_CONTROLLER_COUNT	4
/* Number of I2C ports */
#define I2C_PORT_COUNT		5


/* Number of PWM ports */
#define PWM_COUNT 8

/*****************************************************************************/
/* Memory mapping */
#define CONFIG_RAM_BASE            0x200C0000 /* memory address of data ram */
#define CONFIG_RAM_SIZE            (0x00008000 - 0x800) /* 30KB data ram */
#define CONFIG_PROGRAM_MEMORY_BASE 0x100A8000 /* program memory base address */
#define CONFIG_LPRAM_BASE          0x40001600 /* memory address of lpwr ram */
#define CONFIG_LPRAM_SIZE	   0x00000620 /* 1568B low power ram */

/* System stack size */
#define CONFIG_STACK_SIZE       4096

/* non-standard task stack sizes */
#define IDLE_TASK_STACK_SIZE		512
#define LARGER_TASK_STACK_SIZE		640

#define CHARGER_TASK_STACK_SIZE		640
#define HOOKS_TASK_STACK_SIZE		640
#define CONSOLE_TASK_STACK_SIZE		640

/* Default task stack size */
#define TASK_STACK_SIZE			512

/* Address of RAM log used by Booter */
#define ADDR_BOOT_RAMLOG        0x100C7FC0

/* SPI Flash Spec of W25Q20CV */
#define CONFIG_FLASH_BANK_SIZE	0x00001000  /* protect bank size 4K bytes */
#define CONFIG_FLASH_ERASE_SIZE	0x00001000  /* sector erase size 4K bytes */
#define CONFIG_FLASH_WRITE_SIZE	0x00000001  /* minimum write size */

#define CONFIG_FLASH_WRITE_IDEAL_SIZE 256   /* one page size for write */

#include "config_flash_layout.h"

/* Optional features present on this chip */
#define CONFIG_ADC
#define CONFIG_FPU
#define CONFIG_I2C
#define CONFIG_PECI
#define CONFIG_SWITCH
#define CONFIG_MPU

#define GPIO_PIN(port, index) GPIO_##port, (1 << index)
#define GPIO_PIN_MASK(port, mask) GPIO_##port, (mask)

#endif  /* __CROS_EC_CONFIG_CHIP_H */

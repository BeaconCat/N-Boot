// SPDX-License-Identifier: GPL-2.0+
/*
 * N-Boot warm-reset and NuttX handoff contract for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#include <adc.h>
#include <console.h>
#include <env.h>
#include <nboot_contract.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/delay.h>

static int requested_slot = -1;

#define NBOOT_RECOVERY_ADC_DEVICE	"adc@2ae00000"
#define NBOOT_RECOVERY_ADC_CHANNEL	1
#define NBOOT_RECOVERY_ADC_THRESHOLD	100
#define NBOOT_SERIAL_RECOVERY_TOKEN	'!'
#define NBOOT_SERIAL_RECOVERY_POLLS	10
#define NBOOT_SERIAL_RECOVERY_POLL_US	10000
#define NBOOT_SERIAL_RECOVERY_DRAIN_POLLS	5

static bool nboot_serial_recovery_requested(void)
{
	unsigned int poll;

	for (poll = 0; poll < NBOOT_SERIAL_RECOVERY_POLLS; poll++) {
		while (tstc()) {
			if (getchar() == NBOOT_SERIAL_RECOVERY_TOKEN)
				return true;
		}

		udelay(NBOOT_SERIAL_RECOVERY_POLL_US);
	}

	return false;
}

static void nboot_serial_recovery_drain(void)
{
	unsigned int poll;

	for (poll = 0; poll < NBOOT_SERIAL_RECOVERY_DRAIN_POLLS; poll++) {
		udelay(NBOOT_SERIAL_RECOVERY_POLL_US);
		while (tstc())
			getchar();
	}
}

static bool nboot_recovery_key_pressed(void)
{
	unsigned int value;
	int ret;

	ret = adc_channel_single_shot(NBOOT_RECOVERY_ADC_DEVICE,
				      NBOOT_RECOVERY_ADC_CHANNEL, &value);
	if (ret) {
		log_debug("N-Boot recovery key ADC read failed: %d\n", ret);
		return false;
	}

	return value < NBOOT_RECOVERY_ADC_THRESHOLD;
}

static void nboot_contract_clear_handoff(void)
{
	writel(0, NBOOT_CONTRACT_HANDOFF_REG);
	writel(0, NBOOT_CONTRACT_GENERATION_LO_REG);
	writel(0, NBOOT_CONTRACT_GENERATION_HI_REG);
}

void nboot_contract_init(void)
{
	u32 request = readl(NBOOT_CONTRACT_REQUEST_REG);
	u32 target;
	int disk_target = 0;

	requested_slot = -1;
	writel(0, NBOOT_CONTRACT_REQUEST_REG);
	nboot_contract_clear_handoff();
	if (IS_ENABLED(CONFIG_CMD_BOOTNUTTX))
		disk_target = nboot_bootctrl_take_request();
	if (disk_target > 0)
		request = NBOOT_REBOOT_REQUEST(disk_target);
	if (nboot_serial_recovery_requested()) {
		puts("N-Boot: serial recovery requested, entering console\n");
		nboot_serial_recovery_drain();
		env_set("bootdelay", "-1");
		return;
	}

	if (nboot_recovery_key_pressed()) {
		puts("N-Boot: recovery key pressed, entering Fastboot\n");
		env_set("bootdelay", "-1");
		if (IS_ENABLED(CONFIG_NBOOT_FASTBOOT))
			env_set("preboot", "setenv preboot; fastboot usb 0");
		return;
	}

	if ((request & NBOOT_REBOOT_MAGIC_MASK) != NBOOT_REBOOT_MAGIC)
		return;

	target = request & 0xffU;
	printf("N-Boot: one-shot request %u\n", target);
	switch (target) {
	case NBOOT_REBOOT_CONSOLE:
		env_set("bootdelay", "-1");
		break;
	case NBOOT_REBOOT_FASTBOOT:
		env_set("bootdelay", "-1");
		if (IS_ENABLED(CONFIG_NBOOT_FASTBOOT))
			env_set("preboot", "setenv preboot; fastboot usb 0");
		break;
	case NBOOT_REBOOT_SLOT_A:
		requested_slot = 0;
		break;
	case NBOOT_REBOOT_SLOT_B:
		requested_slot = 1;
		break;
	default:
		break;
	}
}

int nboot_contract_slot_override(void)
{
	return requested_slot;
}

void nboot_contract_write_handoff(enum nboot_boot_medium medium,
				  unsigned int slot, u64 generation,
				  enum nboot_handoff_reason reason)
{
	writel(lower_32_bits(generation), NBOOT_CONTRACT_GENERATION_LO_REG);
	writel(upper_32_bits(generation), NBOOT_CONTRACT_GENERATION_HI_REG);
	writel(NBOOT_HANDOFF_HEADER(medium, slot, reason),
	       NBOOT_CONTRACT_HANDOFF_REG);
}

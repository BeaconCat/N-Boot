// SPDX-License-Identifier: GPL-2.0+
/*
 * N-Boot warm-reset and NuttX handoff contract for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#include <env.h>
#include <nboot_contract.h>
#include <asm/io.h>
#include <linux/kernel.h>

static int requested_slot = -1;

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

	requested_slot = -1;
	writel(0, NBOOT_CONTRACT_REQUEST_REG);
	nboot_contract_clear_handoff();

	if ((request & NBOOT_REBOOT_MAGIC_MASK) != NBOOT_REBOOT_MAGIC)
		return;

	target = request & 0xffU;
	switch (target) {
	case NBOOT_REBOOT_CONSOLE:
		env_set("bootdelay", "-1");
		break;
	case NBOOT_REBOOT_FASTBOOT:
		if (IS_ENABLED(CONFIG_NBOOT_FASTBOOT))
			env_set("preboot", "setenv preboot; fastboot usb 0");
		else
			env_set("bootdelay", "-1");
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

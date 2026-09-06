/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * N-Boot warm-reset and NuttX handoff contract for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#ifndef __NBOOT_CONTRACT_H
#define __NBOOT_CONTRACT_H

#include <linux/types.h>

#define NBOOT_CONTRACT_REQUEST_REG	0x26026230UL
#define NBOOT_CONTRACT_HANDOFF_REG	0x26026234UL
#define NBOOT_CONTRACT_GENERATION_LO_REG	0x26026238UL
#define NBOOT_CONTRACT_GENERATION_HI_REG	0x2602623cUL

#define NBOOT_REBOOT_MAGIC		0x4e425200U
#define NBOOT_REBOOT_MAGIC_MASK		0xffffff00U

#define NBOOT_HANDOFF_MAGIC		0x4e480000U
#define NBOOT_HANDOFF_MAGIC_MASK	0xffff0000U
#define NBOOT_HANDOFF_VERSION		2U

enum nboot_reboot_target {
	NBOOT_REBOOT_NONE = 0,
	NBOOT_REBOOT_CONSOLE = 1,
	NBOOT_REBOOT_FASTBOOT = 2,
	NBOOT_REBOOT_SLOT_A = 3,
	NBOOT_REBOOT_SLOT_B = 4,
};

enum nboot_handoff_reason {
	NBOOT_HANDOFF_NORMAL = 0,
	NBOOT_HANDOFF_REQUESTED_SLOT = 1,
	NBOOT_HANDOFF_FALLBACK = 2,
};

enum nboot_boot_medium {
	NBOOT_MEDIUM_UNKNOWN = 0,
	NBOOT_MEDIUM_SD = 1,
	NBOOT_MEDIUM_EMMC = 2,
};

#define NBOOT_REBOOT_REQUEST(target) \
	(NBOOT_REBOOT_MAGIC | ((target) & 0xffU))

#define NBOOT_HANDOFF_HEADER(medium, slot, reason) \
	(NBOOT_HANDOFF_MAGIC | (NBOOT_HANDOFF_VERSION << 12) | \
	 (((reason) & 0xfU) << 8) | (((medium) & 0xfU) << 4) | \
	 ((slot) & 0xfU))

void nboot_contract_init(void);
int nboot_contract_slot_override(void);
void nboot_contract_write_handoff(enum nboot_boot_medium medium,
				  unsigned int slot, u64 generation,
				  enum nboot_handoff_reason reason);

#endif /* __NBOOT_CONTRACT_H */

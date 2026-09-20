/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2026 Pharos Tech */
#ifndef __NBOOT_AMP_H
#define __NBOOT_AMP_H

#include <linux/types.h>

#define NBOOT_AMP_LINUX_ENTRY 0x42000000UL
#define NBOOT_AMP_NUTTX_ENTRY 0x4a400000UL
#define NBOOT_AMP_LINUX_CPU 0x100UL
#define NBOOT_AMP_MAX_FIT_SIZE 0x20000000UL
#define NBOOT_AMP_ABI_VERSION 2
#define NBOOT_AMP_UART_IRQ 108
#define NBOOT_AMP_MBOX_IRQ 174
#define NBOOT_AMP_IRQ_PRIORITY 0x80

/* Upper bound on the interrupt routes the AMP device tree may claim.  The
 * console and the transport are required; the rest are peripherals the
 * control domain also owns.  The limit exists so a malformed or hostile tree
 * cannot make the validator walk an unbounded list.
 */
#define NBOOT_AMP_MAX_IRQ_ROUTES 16
#define NBOOT_AMP_VRING0_BASE 0x47800000UL
#define NBOOT_AMP_VRING_NUM 64
#define NBOOT_AMP_BUFFER_BASE 0x47a00000UL
#define NBOOT_AMP_BUFFER_LIMIT 0x00200000UL
#define NBOOT_AMP_BUFFER_SIZE 512
#define NBOOT_AMP_STAGE_ADDRESS 0x60000000UL
#define NBOOT_AMP_STAGE_SIZE 0x04000000UL

/* Validate the complete external-data FIT before any payload is loaded. */
int nboot_amp_validate(const void *fit, u32 size);
int nboot_amp_validate_fdt(const void *fdt, u32 size);

/* Where a loaded AMP image came from, for the handoff to the control domain.
 * medium is an enum nboot_boot_medium value; generation is the bootctrl
 * generation after any trial accounting.
 */
struct nboot_amp_origin {
	unsigned int medium;
	unsigned int slot;
	u64 generation;
};

/* Load the active AMP slot, consuming an unconfirmed trial before boot. */
int nboot_amp_load_slot(ulong address, ulong capacity, ulong *size,
			struct nboot_amp_origin *origin);

/* The medium that holds bootctrl, for an image that came from RAM. */
int nboot_amp_boot_medium(unsigned int *medium);

#endif

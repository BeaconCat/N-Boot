/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * N-Boot storage selection for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#ifndef __NBOOT_STORAGE_H
#define __NBOOT_STORAGE_H

#include <blk.h>
#include <mmc.h>
#include <nboot_contract.h>

#define NBOOT_LAYOUT_BLOCK_SIZE		512
#define NBOOT_LAYOUT_UBOOT_START	0x4000
#define NBOOT_LAYOUT_UBOOT_BLOCKS	8192
#define NBOOT_LAYOUT_TRUST_START	0x6000
#define NBOOT_LAYOUT_TRUST_BLOCKS	8192
#define NBOOT_LAYOUT_BOOTCTRL_START	0x8000
#define NBOOT_LAYOUT_BOOTCTRL_BLOCKS	2048
#define NBOOT_LAYOUT_NUTTX_A_START	0x9000
#define NBOOT_LAYOUT_NUTTX_B_START	0x29000
#define NBOOT_LAYOUT_NUTTX_BLOCKS	131072

struct nboot_storage {
	enum nboot_boot_medium medium;
	int devnum;
	struct mmc *mmc;
	struct blk_desc *desc;
};

int nboot_storage_boot_devnum(void);
int nboot_storage_open(int devnum, struct nboot_storage *storage);
int nboot_storage_open_boot(struct nboot_storage *storage);
void nboot_storage_reset_target(void);
int nboot_storage_set_target(enum nboot_boot_medium medium);
const char *nboot_storage_target_name(void);

#endif /* __NBOOT_STORAGE_H */

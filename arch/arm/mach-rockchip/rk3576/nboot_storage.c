// SPDX-License-Identifier: GPL-2.0+
/*
 * N-Boot storage selection for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#include <fb_mmc.h>
#include <env.h>
#include <nboot_storage.h>
#include <part.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#define NBOOT_SD_DEVNUM		0
#define NBOOT_EMMC_DEVNUM	1

struct nboot_layout_partition {
	const char *name;
	lbaint_t start;
	lbaint_t blocks;
};

static const struct nboot_layout_partition nboot_layout[] = {
	{ "uboot", NBOOT_LAYOUT_UBOOT_START, NBOOT_LAYOUT_UBOOT_BLOCKS },
	{ "trust", NBOOT_LAYOUT_TRUST_START, NBOOT_LAYOUT_TRUST_BLOCKS },
	{ "bootctrl", NBOOT_LAYOUT_BOOTCTRL_START,
	  NBOOT_LAYOUT_BOOTCTRL_BLOCKS },
	{ "nuttx_a", NBOOT_LAYOUT_NUTTX_A_START, NBOOT_LAYOUT_NUTTX_BLOCKS },
	{ "nuttx_b", NBOOT_LAYOUT_NUTTX_B_START, NBOOT_LAYOUT_NUTTX_BLOCKS },
};

static int nboot_storage_check_partition(struct blk_desc *desc,
					 const char *name, lbaint_t start,
					 lbaint_t blocks)
{
	struct disk_partition partition;

	if (part_get_info_by_name(desc, name, &partition) < 0)
		return -ENOENT;
	if (partition.start != start || partition.size != blocks)
		return -EINVAL;

	return 0;
}

static int nboot_storage_check_layout(struct blk_desc *desc)
{
	int index;
	int ret;

	if (desc->blksz != NBOOT_LAYOUT_BLOCK_SIZE)
		return -EPROTONOSUPPORT;

	for (index = 0; index < ARRAY_SIZE(nboot_layout); index++) {
		const struct nboot_layout_partition *partition =
			&nboot_layout[index];

		ret = nboot_storage_check_partition(desc, partition->name,
						    partition->start,
						    partition->blocks);
		if (ret)
			return ret;
	}

	return 0;
}

static int selected_devnum = -1;
static int target_devnum = -1;

static int nboot_storage_preferred_devnum(void)
{
	return NBOOT_SD_DEVNUM;
}

int nboot_storage_boot_devnum(void)
{
	struct nboot_storage storage;
	int primary;
	int alternate;

	if (target_devnum >= 0)
		return target_devnum;
	if (selected_devnum >= 0)
		return selected_devnum;

	primary = nboot_storage_preferred_devnum();
	alternate = primary == NBOOT_SD_DEVNUM ?
		NBOOT_EMMC_DEVNUM : NBOOT_SD_DEVNUM;
	if (!nboot_storage_open(primary, &storage))
		return selected_devnum;
	if (!nboot_storage_open(alternate, &storage))
		return selected_devnum;

	return primary;
}

int nboot_storage_open(int devnum, struct nboot_storage *storage)
{
	int ret;

	if (!storage || (devnum != NBOOT_SD_DEVNUM &&
			 devnum != NBOOT_EMMC_DEVNUM))
		return -EINVAL;

	storage->mmc = find_mmc_device(devnum);
	if (!storage->mmc)
		return -ENODEV;
	ret = mmc_init(storage->mmc);
	if (ret)
		return ret;

	storage->desc = mmc_get_blk_desc(storage->mmc);
	if (!storage->desc)
		return -ENODEV;
	ret = nboot_storage_check_layout(storage->desc);
	if (ret)
		return ret;

	storage->devnum = devnum;
	storage->medium = devnum == NBOOT_EMMC_DEVNUM ?
		NBOOT_MEDIUM_EMMC : NBOOT_MEDIUM_SD;
	selected_devnum = devnum;
	env_set("fastboot.nboot-medium",
		storage->medium == NBOOT_MEDIUM_EMMC ? "emmc" : "sd");
	return 0;
}

int nboot_storage_open_boot(struct nboot_storage *storage)
{
	int primary;
	int alternate;
	int ret;

	if (target_devnum >= 0)
		return nboot_storage_open(target_devnum, storage);
	if (selected_devnum >= 0)
		return nboot_storage_open(selected_devnum, storage);

	primary = nboot_storage_preferred_devnum();
	alternate = primary == NBOOT_SD_DEVNUM ?
		NBOOT_EMMC_DEVNUM : NBOOT_SD_DEVNUM;
	ret = nboot_storage_open(primary, storage);
	if (!ret)
		return 0;

	return nboot_storage_open(alternate, storage);
}

void nboot_storage_reset_target(void)
{
	target_devnum = -1;
	env_set("fastboot.nboot-medium", nboot_storage_target_name());
}

int nboot_storage_set_target(enum nboot_boot_medium medium)
{
	switch (medium) {
	case NBOOT_MEDIUM_UNKNOWN:
		target_devnum = -1;
		env_set("fastboot.nboot-medium", nboot_storage_target_name());
		return 0;
	case NBOOT_MEDIUM_SD:
		target_devnum = NBOOT_SD_DEVNUM;
		env_set("fastboot.nboot-medium", "sd");
		return 0;
	case NBOOT_MEDIUM_EMMC:
		target_devnum = NBOOT_EMMC_DEVNUM;
		env_set("fastboot.nboot-medium", "emmc");
		return 0;
	default:
		return -EINVAL;
	}
}

const char *nboot_storage_target_name(void)
{
	int devnum = target_devnum >= 0 ? target_devnum :
		nboot_storage_boot_devnum();

	return devnum == NBOOT_EMMC_DEVNUM ? "emmc" : "sd";
}

int fastboot_mmc_get_devnum(void)
{
	return nboot_storage_boot_devnum();
}

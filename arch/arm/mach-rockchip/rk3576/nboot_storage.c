// SPDX-License-Identifier: GPL-2.0+
/*
 * N-Boot storage selection for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#include <fb_mmc.h>
#include <env.h>
#include <memalign.h>
#include <nboot_storage.h>
#include <part.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#define NBOOT_SD_DEVNUM		0
#define NBOOT_EMMC_DEVNUM	1

/* 200, 400, 800, 1600 and 3200 ms: a little over six seconds in all. */
#define NBOOT_STORAGE_RETRIES	5
#define NBOOT_STORAGE_RETRY_MS	200
#define NBOOT_STORAGE_FULL_SPEED_RETRIES	2
#define NBOOT_STORAGE_MAX_BLOCK	4096

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
static unsigned int retries_left = NBOOT_STORAGE_RETRIES;

/* Tell a medium that cannot be read from one whose content is not ours. */
static bool nboot_storage_readable(struct blk_desc *desc)
{
	if (!desc->blksz || desc->blksz > NBOOT_STORAGE_MAX_BLOCK)
		return true;

	ALLOC_CACHE_ALIGN_BUFFER(u8, block, desc->blksz);

	return blk_dread(desc, 0, 1, block) == 1;
}

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
	if (ret) {
		/* An empty SD slot is the normal case and says nothing.  The
		 * eMMC is soldered down: when it does not initialise, the
		 * medium is there and is not answering.
		 */
		if (devnum != NBOOT_EMMC_DEVNUM)
			return ret;
		printf("N-Boot: eMMC did not initialise (%d)\n", ret);
		return -EIO;
	}

	storage->desc = mmc_get_blk_desc(storage->mmc);
	if (!storage->desc)
		return -ENODEV;
	ret = nboot_storage_check_layout(storage->desc);
	if (ret) {
		/* A partition that is not found looks the same whether the
		 * table is foreign or could not be read.  One block settles it.
		 */
		if (nboot_storage_readable(storage->desc))
			return ret;
		printf("N-Boot: mmc%d initialised but does not read (%d)\n",
		       devnum, ret);
		return -EIO;
	}

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
	int other;
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
	other = nboot_storage_open(alternate, storage);
	if (!other)
		return 0;

	/* Either medium not answering makes the whole answer provisional. */
	return ret == -EIO ? ret : other;
}

bool nboot_storage_retry(const char *who, int error)
{
	int only = target_devnum >= 0 ? target_devnum : selected_devnum;
	unsigned int attempt;
	unsigned int delay;
	int devnum;

	if (error != -EIO || !retries_left)
		return false;

	attempt = NBOOT_STORAGE_RETRIES - retries_left + 1;
	delay = NBOOT_STORAGE_RETRY_MS << (attempt - 1);
	retries_left--;
	printf("%s: boot medium not ready (%d); re-initialising, retry %u/%u in %u ms\n",
	       who, error, attempt, NBOOT_STORAGE_RETRIES, delay);
	mdelay(delay);

	/* What "mmc dev" does: forget the card, so that the next mmc_init()
	 * resets the host and starts over from CMD0.  That also takes a card
	 * out of the data state a failed multi-block read leaves it in.
	 */
	for (devnum = NBOOT_SD_DEVNUM; devnum <= NBOOT_EMMC_DEVNUM; devnum++) {
		struct mmc *mmc = find_mmc_device(devnum);

		if (!mmc || (only >= 0 && only != devnum))
			continue;
		mmc->has_init = 0;

		/* Two attempts at full speed are enough to tell a slow card
		 * from a link that does not hold: from the third on, the eMMC
		 * comes up in 52 MHz high-speed mode, which needs no tuning.
		 * The mode lasts for this run of N-Boot only.
		 */
		if (IS_ENABLED(CONFIG_MMC_SPEED_MODE_SET) &&
		    devnum == NBOOT_EMMC_DEVNUM &&
		    attempt > NBOOT_STORAGE_FULL_SPEED_RETRIES &&
		    mmc->user_speed_mode != MMC_HS_52) {
			puts("N-Boot: eMMC limited to 52 MHz for this boot\n");
			mmc->user_speed_mode = MMC_HS_52;
		}
	}

	return true;
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

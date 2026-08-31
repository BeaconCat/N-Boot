// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 Pharos Tech
 */

#include <blk.h>
#include <command.h>
#include <cpu_func.h>
#include <hash.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <part.h>
#include <u-boot/crc.h>
#include <vsprintf.h>
#include <asm/cache.h>
#include <asm/u-boot-arm.h>
#include <linux/bitops.h>
#include <linux/errno.h>

#define K7_BOOTCTRL_MAGIC        "K7ABCTRL"
#define K7_BOOTCTRL_VERSION      1
#define K7_BOOTCTRL_RECORD_SIZE  4096
#define K7_BOOTCTRL_COPY_COUNT   2
#define K7_BOOTCTRL_BLOCK_SIZE   512
#define K7_BOOTCTRL_BLOCKS       8
#define K7_NUTTX_LOAD_ADDR       0x40200000UL
#define K7_NUTTX_DOMAIN          0
#define K7_SHA256_SIZE           32

struct k7_slot_disk {
	u8 priority;
	u8 tries_remaining;
	u8 successful;
	u8 reserved;
	__le64 image_size;
	__le64 image_version;
	u8 sha256[K7_SHA256_SIZE];
} __packed;

struct k7_domain_disk {
	u8 active_slot;
	u8 reserved[3];
	struct k7_slot_disk slots[2];
} __packed;

struct k7_bootctrl_disk {
	u8 magic[8];
	__le16 format_version;
	__le16 header_size;
	__le64 generation;
	struct k7_domain_disk domains[2];
	u8 padding[K7_BOOTCTRL_RECORD_SIZE - 4 - 20 -
		   sizeof(struct k7_domain_disk) * 2];
	__le32 crc32;
} __packed;

_Static_assert(sizeof(struct k7_bootctrl_disk) == K7_BOOTCTRL_RECORD_SIZE,
	       "bootctrl record size changed");

static bool k7_bootctrl_valid(const struct k7_bootctrl_disk *record)
{
	u32 expected;
	u32 actual;

	if (memcmp(record->magic, K7_BOOTCTRL_MAGIC, sizeof(record->magic)))
		return false;
	if (le16_to_cpu(record->format_version) != K7_BOOTCTRL_VERSION)
		return false;
	if (le16_to_cpu(record->header_size) != 20)
		return false;

	expected = le32_to_cpu(record->crc32);
	actual = crc32(0, (const u8 *)record,
		       offsetof(struct k7_bootctrl_disk, crc32));
	return expected == actual;
}

static int k7_bootctrl_read(struct blk_desc *desc,
			    const struct disk_partition *partition,
			    struct k7_bootctrl_disk *records,
			    int *selected)
{
	int index;
	int best = -1;

	if (desc->blksz != K7_BOOTCTRL_BLOCK_SIZE)
		return -EPROTONOSUPPORT;

	for (index = 0; index < K7_BOOTCTRL_COPY_COUNT; index++) {
		if (blk_dread(desc, partition->start + index * K7_BOOTCTRL_BLOCKS,
			      K7_BOOTCTRL_BLOCKS, &records[index]) !=
		    K7_BOOTCTRL_BLOCKS)
			continue;
		if (!k7_bootctrl_valid(&records[index]))
			continue;
		if (best < 0 ||
		    le64_to_cpu(records[index].generation) >
		    le64_to_cpu(records[best].generation))
			best = index;
	}

	if (best < 0)
		return -EBADMSG;
	*selected = best;
	return 0;
}

static int k7_bootctrl_choose(const struct k7_domain_disk *domain)
{
	int active = domain->active_slot;
	int best = -1;
	int index;

	for (index = 0; index < 2; index++) {
		const struct k7_slot_disk *slot = &domain->slots[index];

		if (!slot->priority)
			continue;
		if (!slot->successful && !slot->tries_remaining)
			continue;
		if (best < 0 || slot->priority > domain->slots[best].priority ||
		    (slot->priority == domain->slots[best].priority &&
		     index == active))
			best = index;
	}

	return best;
}

static void k7_bootctrl_finalize(struct k7_bootctrl_disk *record)
{
	u32 checksum;

	record->generation = cpu_to_le64(le64_to_cpu(record->generation) + 1);
	checksum = crc32(0, (const u8 *)record,
			 offsetof(struct k7_bootctrl_disk, crc32));
	record->crc32 = cpu_to_le32(checksum);
}

static int k7_bootctrl_write(struct blk_desc *desc,
			     const struct disk_partition *partition,
			     struct k7_bootctrl_disk *records, int selected)
{
	struct k7_bootctrl_disk verify;
	int first = 1 - selected;
	int second = selected;

	k7_bootctrl_finalize(&records[selected]);
	if (blk_dwrite(desc, partition->start + first * K7_BOOTCTRL_BLOCKS,
		       K7_BOOTCTRL_BLOCKS, &records[selected]) !=
	    K7_BOOTCTRL_BLOCKS)
		return -EIO;
	if (blk_dread(desc, partition->start + first * K7_BOOTCTRL_BLOCKS,
		      K7_BOOTCTRL_BLOCKS, &verify) != K7_BOOTCTRL_BLOCKS ||
	    !k7_bootctrl_valid(&verify) ||
	    verify.generation != records[selected].generation)
		return -EIO;

	if (blk_dwrite(desc, partition->start + second * K7_BOOTCTRL_BLOCKS,
		       K7_BOOTCTRL_BLOCKS, &records[selected]) !=
	    K7_BOOTCTRL_BLOCKS)
		printf("bootnuttx: warning: second bootctrl copy was not updated\n");
	records[first] = records[selected];
	records[second] = records[selected];
	return 0;
}

static int k7_nuttx_partition(struct blk_desc *desc, int slot,
			      struct disk_partition *partition)
{
	int ret;

	ret = part_get_info_by_name(desc, slot ? "nuttx_b" : "nuttx_a",
				    partition);
	return ret < 0 ? ret : 0;
}

static int k7_nuttx_load(struct blk_desc *desc,
			 const struct disk_partition *partition,
			 const struct k7_slot_disk *slot)
{
	u8 digest[K7_SHA256_SIZE];
	int digest_size = sizeof(digest);
	u64 image_size = le64_to_cpu(slot->image_size);
	lbaint_t blocks;
	void *image;
	int ret;

	if (!image_size || image_size > partition->size * desc->blksz ||
	    image_size > UINT_MAX)
		return -EFBIG;

	blocks = DIV_ROUND_UP(image_size, desc->blksz);
	image = map_sysmem(K7_NUTTX_LOAD_ADDR, image_size);
	if (blk_dread(desc, partition->start, blocks, image) != blocks) {
		unmap_sysmem(image);
		return -EIO;
	}

	ret = hash_block("sha256", image, image_size, digest, &digest_size);
	if (ret || digest_size != K7_SHA256_SIZE ||
	    memcmp(digest, slot->sha256, K7_SHA256_SIZE)) {
		unmap_sysmem(image);
		return -EKEYREJECTED;
	}

	flush_cache(K7_NUTTX_LOAD_ADDR, ALIGN(image_size, ARCH_DMA_MINALIGN));
	unmap_sysmem(image);
	return 0;
}

static int do_bootnuttx(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct k7_bootctrl_disk *records;
	struct disk_partition bootctrl;
	struct disk_partition image;
	struct k7_domain_disk *domain;
	struct blk_desc *desc;
	struct mmc *mmc;
	int selected;
	int devnum = 0;
	int attempt;
	int ret;

	if (argc > 2)
		return CMD_RET_USAGE;
	if (argc == 2)
		devnum = dectoul(argv[1], NULL);

	mmc = find_mmc_device(devnum);
	if (!mmc || mmc_init(mmc)) {
		printf("bootnuttx: MMC device %d is unavailable\n", devnum);
		return CMD_RET_FAILURE;
	}
	desc = mmc_get_blk_desc(mmc);
	if (part_get_info_by_name(desc, "bootctrl", &bootctrl) < 0) {
		printf("bootnuttx: bootctrl partition is missing\n");
		return CMD_RET_FAILURE;
	}

	records = memalign(ARCH_DMA_MINALIGN,
			   sizeof(*records) * K7_BOOTCTRL_COPY_COUNT);
	if (!records)
		return CMD_RET_FAILURE;

	ret = k7_bootctrl_read(desc, &bootctrl, records, &selected);
	if (ret) {
		printf("bootnuttx: no valid bootctrl copy (%d)\n", ret);
		free(records);
		return CMD_RET_FAILURE;
	}

	for (attempt = 0; attempt < 2; attempt++) {
		bool metadata_dirty;
		int chosen;
		struct k7_slot_disk *slot;

		domain = &records[selected].domains[K7_NUTTX_DOMAIN];
		chosen = k7_bootctrl_choose(domain);
		if (chosen < 0)
			break;
		slot = &domain->slots[chosen];
		metadata_dirty = domain->active_slot != chosen;
		domain->active_slot = chosen;

		if (!slot->successful) {
			slot->tries_remaining--;
			metadata_dirty = true;
		}
		if (metadata_dirty) {
			ret = k7_bootctrl_write(desc, &bootctrl, records,
						selected);
			if (ret)
				break;
		}

		ret = k7_nuttx_partition(desc, chosen, &image);
		if (!ret)
			ret = k7_nuttx_load(desc, &image, slot);
		if (!ret) {
			void (*entry)(void) = (void *)K7_NUTTX_LOAD_ADDR;

			printf("bootnuttx: booting NuttX slot %c, version %llu\n",
			       'a' + chosen, le64_to_cpu(slot->image_version));
			free(records);
			cleanup_before_linux();
			entry();
			return CMD_RET_FAILURE;
		}

		printf("bootnuttx: slot %c rejected (%d)\n", 'a' + chosen,
		       ret);
		slot->priority = 0;
		slot->tries_remaining = 0;
		if (k7_bootctrl_write(desc, &bootctrl, records, selected))
			break;
	}

	free(records);
	printf("bootnuttx: no bootable NuttX slot remains\n");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(bootnuttx, 2, 0, do_bootnuttx,
	   "boot a verified NuttX A/B slot",
	   "[mmc-device]\n"
	   "    - load the selected NuttX slot at 0x40200000 and branch to it");

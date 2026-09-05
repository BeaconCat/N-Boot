// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 Pharos Tech
 */

#include <blk.h>
#include <command.h>
#include <cpu_func.h>
#include <fastboot.h>
#include <hash.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <nboot_recovery.h>
#include <part.h>
#include <u-boot/crc.h>
#include <u-boot/sha256.h>
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
#define K7_RECOVERY_CHUNK        (64 * 1024)

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
	if (partition->size < K7_BOOTCTRL_BLOCKS * K7_BOOTCTRL_COPY_COUNT)
		return -EINVAL;

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

#if IS_ENABLED(CONFIG_NBOOT_FASTBOOT)
/* Hash from media, not from the download buffer. No slot is activated here. */
static int k7_recovery_hash(struct blk_desc *desc,
			    const struct disk_partition *part, u64 size,
			    u8 digest[K7_SHA256_SIZE])
{
	sha256_context ctx;
	u8 *buffer;
	u64 offset = 0;
	int ret = 0;

	if (!size || desc->blksz != K7_BOOTCTRL_BLOCK_SIZE ||
	    DIV_ROUND_UP(size, desc->blksz) > part->size)
		return -EFBIG;
	buffer = memalign(ARCH_DMA_MINALIGN, K7_RECOVERY_CHUNK);
	if (!buffer)
		return -ENOMEM;
	sha256_starts(&ctx);
	while (offset < size) {
		u32 bytes = min_t(u64, size - offset, K7_RECOVERY_CHUNK);
		lbaint_t blocks = DIV_ROUND_UP(bytes, desc->blksz);

		if (blk_dread(desc, part->start + offset / desc->blksz,
			      blocks, buffer) != blocks) {
			ret = -EIO;
			break;
		}
		sha256_update(&ctx, buffer, bytes);
		offset += bytes;
	}
	if (!ret)
		sha256_finish(&ctx, digest);
	free(buffer);
	return ret;
}

static int k7_recovery_flash(struct blk_desc *desc,
			     const struct disk_partition *control,
			     const struct disk_partition *part,
			     struct k7_bootctrl_disk *records, int selected,
			     struct k7_slot_disk *slot, const void *data,
			     u32 size)
{
	u8 digest[K7_SHA256_SIZE], actual[K7_SHA256_SIZE];
	u8 *tail;
	lbaint_t blocks = size / desc->blksz;
	u32 remainder = size % desc->blksz;
	int digest_size = sizeof(digest);
	u64 version = le64_to_cpu(slot->image_version) + 1;
	int ret;

	if (!size || DIV_ROUND_UP((u64)size, desc->blksz) > part->size)
		return -EFBIG;
	if (size < 60 || memcmp((const u8 *)data + 56, "ARMd", 4))
		return -ENOEXEC;
	ret = hash_block("sha256", data, size, digest, &digest_size);
	if (ret || digest_size != sizeof(digest))
		return -EINVAL;
	tail = memalign(ARCH_DMA_MINALIGN, desc->blksz);
	if (!tail)
		return -ENOMEM;
	/* Invalidate before the first payload write, including on interrupted OTA. */
	memset(slot, 0, sizeof(*slot));
	ret = k7_bootctrl_write(desc, control, records, selected);
	if (ret)
		goto out;
	if (blocks && blk_dwrite(desc, part->start, blocks, data) != blocks) {
		ret = -EIO;
		goto out;
	}
	if (remainder) {
		memset(tail, 0, desc->blksz);
		memcpy(tail, (const u8 *)data + blocks * desc->blksz, remainder);
		if (blk_dwrite(desc, part->start + blocks, 1, tail) != 1) {
			ret = -EIO;
			goto out;
		}
	}
	ret = k7_recovery_hash(desc, part, size, actual);
	if (ret || memcmp(actual, digest, sizeof(digest))) {
		ret = ret ? ret : -EKEYREJECTED;
		goto out;
	}
	/* Keep priority zero: activation is a separate, verified operation. */
	slot->image_size = cpu_to_le64(size);
	slot->image_version = cpu_to_le64(version);
	memcpy(slot->sha256, digest, sizeof(digest));
	ret = k7_bootctrl_write(desc, control, records, selected);
out:
	free(tail);
	return ret;
}

void fastboot_oem_board(char *parameter, void *data, u32 size, char *response)
{
	static const char * const names[] = {
		"nuttx_a", "nuttx_b"
	};
	struct k7_bootctrl_disk *records;
	struct disk_partition control, part;
	struct k7_domain_disk *domain;
	struct k7_slot_disk *slot;
	struct blk_desc *desc;
	struct mmc *mmc;
	u8 digest[K7_SHA256_SIZE];
	const char *name;
	bool flash;
	int i, selected, ret;

	if (!parameter) {
		fastboot_fail("expected flash:<slot> or activate:<slot>", response);
		return;
	}
	if (nboot_recovery_unlock(parameter, response))
		return;
	flash = !strncmp(parameter, "flash:", 6);
	if (!flash && strncmp(parameter, "activate:", 9)) {
		fastboot_fail("unsupported recovery operation", response);
		return;
	}
	name = parameter + (flash ? 6 : 9);
	for (i = 0; i < ARRAY_SIZE(names); i++)
		if (!strcmp(name, names[i]))
			break;
	if (i == ARRAY_SIZE(names)) {
		fastboot_fail("partition is not in recovery allowlist", response);
		return;
	}
	mmc = find_mmc_device(0);
	if (!mmc || mmc_init(mmc)) {
		fastboot_fail("SD device unavailable", response);
		return;
	}
	desc = mmc_get_blk_desc(mmc);
	if (desc->blksz != K7_BOOTCTRL_BLOCK_SIZE ||
	    part_get_info_by_name(desc, "bootctrl", &control) < 0 ||
	    part_get_info_by_name(desc, name, &part) < 0) {
		fastboot_fail("invalid recovery partition layout", response);
		return;
	}
	records = memalign(ARCH_DMA_MINALIGN, sizeof(*records) * 2);
	if (!records) {
		fastboot_fail("out of memory", response);
		return;
	}
	ret = k7_bootctrl_read(desc, &control, records, &selected);
	if (ret)
		goto out;
	domain = &records[selected].domains[i / 2];
	slot = &domain->slots[i % 2];
	if (flash) {
		if (k7_bootctrl_choose(domain) == i % 2 &&
		    !nboot_recovery_authorized()) {
			ret = -EBUSY;
			goto out;
		}
		ret = k7_recovery_flash(desc, &control, &part, records,
					selected, slot, data, size);
	} else {
		ret = k7_recovery_hash(desc, &part,
				       le64_to_cpu(slot->image_size), digest);
		if (ret || memcmp(digest, slot->sha256, sizeof(digest))) {
			ret = ret ? ret : -EKEYREJECTED;
			goto out;
		}
		domain->active_slot = i % 2;
		slot->priority = 15;
		slot->tries_remaining = 3;
		slot->successful = 0;
		if (domain->slots[1 - i % 2].priority == 15)
			domain->slots[1 - i % 2].priority = 14;
		ret = k7_bootctrl_write(desc, &control, records, selected);
	}
out:
	free(records);
	if (ret) {
		char error[48];

		snprintf(error, sizeof(error), "recovery rejected (%d)", ret);
		fastboot_fail(error, response);
	} else {
		fastboot_okay(flash ? "verified; activate separately" : "slot activated",
			      response);
	}
}
#endif

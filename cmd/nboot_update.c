// SPDX-License-Identifier: GPL-2.0+
/*
 * Verified redundant N-Boot FIT updates for KICKPI-K7.
 *
 * Copyright 2026 Pharos Tech
 */

#include <asm/unaligned.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <blk.h>
#include <cpu_func.h>
#include <image.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <malloc.h>
#include <mmc.h>
#include <nboot_recovery.h>
#include <nboot_update.h>
#include <part.h>

DECLARE_GLOBAL_DATA_PTR;

#define NBOOT_UPDATE_BLOCK_SIZE       512
#define NBOOT_UPDATE_FIT_SIZE         (4 * 1024 * 1024)
#define NBOOT_UPDATE_SECTOR           0x4000
#define NBOOT_UPDATE_SLOT_BLOCKS      \
	(NBOOT_UPDATE_FIT_SIZE / NBOOT_UPDATE_BLOCK_SIZE)
#define NBOOT_UPDATE_LOAD_ADDR        0x40200000UL
#define NBOOT_UPDATE_TEXT_OFFSET      0x200000ULL
#define NBOOT_UPDATE_HEADER_SIZE      64
#define NBOOT_UPDATE_IMAGE_COUNT      6

struct nboot_update_range {
	u32 position;
	u32 size;
};

struct nboot_update_image {
	const char *name;
	int node;
	const u8 *data;
	u32 size;
	struct nboot_update_range range;
};

static const char * const nboot_update_images[NBOOT_UPDATE_IMAGE_COUNT] = {
	"atf-1", "uboot", "fdt", "atf-2", "atf-3", "optee"
};

static int nboot_update_find_image(const char *name)
{
	int index;

	for (index = 0; index < NBOOT_UPDATE_IMAGE_COUNT; index++)
		if (!strcmp(name, nboot_update_images[index]))
			return index;

	return -ENOENT;
}

static bool nboot_update_string_equal(const char *value, int length,
				      const char *expected)
{
	int expected_length = strlen(expected) + 1;

	return length == expected_length && !memcmp(value, expected, length);
}

static int nboot_update_check_loadables(const char *list, int length)
{
	static const char * const expected[] = {
		"uboot", "atf-2", "atf-3", "optee"
	};
	int found = 0;
	int offset = 0;

	while (offset < length) {
		int entry_length = strnlen(list + offset, length - offset);
		int index;

		if (entry_length == length - offset)
			return -EINVAL;
		for (index = 0; index < ARRAY_SIZE(expected); index++)
			if (nboot_update_string_equal(list + offset,
						      entry_length + 1,
						      expected[index]))
				break;
		if (index == ARRAY_SIZE(expected) || (found & BIT(index)))
			return -EINVAL;
		found |= BIT(index);
		offset += entry_length + 1;
	}

	return found == GENMASK(ARRAY_SIZE(expected) - 1, 0) ? 0 : -EINVAL;
}

static int nboot_update_check_hash(const void *fit, int image_node,
				   const void *data, u32 size)
{
	int hash_node;
	bool sha256_found = false;

	fdt_for_each_subnode(hash_node, fit, image_node) {
		const char *name;
		const char *algo;
		int length;

		name = fit_get_name(fit, hash_node, NULL);
		if (strncmp(name, FIT_HASH_NODENAME, strlen(FIT_HASH_NODENAME)))
			continue;
		algo = fdt_getprop(fit, hash_node, FIT_ALGO_PROP, &length);
		if (!algo || !nboot_update_string_equal(algo, length, "sha256"))
			continue;
		sha256_found = true;
	}

	if (!sha256_found)
		return -EKEYREJECTED;
	if (!fit_image_verify_with_data(fit, image_node, gd_fdt_blob(), data,
					size))
		return -EKEYREJECTED;

	return 0;
}

static int nboot_update_collect_images(const void *fit, u32 fit_size,
				       struct nboot_update_image images[])
{
	u32 header_size = fdt_totalsize(fit);
	int images_node;
	int node;
	int count = 0;

	images_node = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images_node < 0)
		return -ENOENT;

	fdt_for_each_subnode(node, fit, images_node) {
		const char *name = fit_get_name(fit, node, NULL);
		int index = nboot_update_find_image(name);
		int position;
		int data_size;

		if (index < 0 || images[index].node >= 0)
			return -EINVAL;
		if (fit_image_get_data_position(fit, node, &position) ||
		    fit_image_get_data_size(fit, node, &data_size))
			return -ENOENT;
		if (position < 0 || data_size <= 0 || (u32)position < header_size ||
		    (u32)position > fit_size || (u32)data_size > fit_size - position)
			return -EFBIG;

		images[index].node = node;
		images[index].data = (const u8 *)fit + position;
		images[index].size = data_size;
		images[index].range.position = position;
		images[index].range.size = data_size;
		count++;
	}

	return count == NBOOT_UPDATE_IMAGE_COUNT ? 0 : -ENOENT;
}

static int nboot_update_check_ranges(const struct nboot_update_image images[])
{
	int left;
	int right;

	for (left = 0; left < NBOOT_UPDATE_IMAGE_COUNT; left++) {
		u32 left_end = images[left].range.position + images[left].range.size;

		for (right = left + 1; right < NBOOT_UPDATE_IMAGE_COUNT; right++) {
			u32 right_end = images[right].range.position +
					images[right].range.size;

			if (images[left].range.position < right_end &&
			    images[right].range.position < left_end)
				return -EINVAL;
		}
	}

	return 0;
}

static int nboot_update_check_configuration(const void *fit)
{
	const char *default_name;
	const char *property;
	int configurations;
	int configuration;
	int length;

	configurations = fdt_path_offset(fit, FIT_CONFS_PATH);
	if (configurations < 0)
		return -ENOENT;
	default_name = fdt_getprop(fit, configurations, FIT_DEFAULT_PROP,
				   &length);
	if (!default_name || !length || default_name[length - 1])
		return -EINVAL;
	configuration = fdt_subnode_offset(fit, configurations, default_name);
	if (configuration < 0)
		return -ENOENT;

	property = fdt_getprop(fit, configuration, FIT_FIRMWARE_PROP, &length);
	if (!property || !nboot_update_string_equal(property, length, "atf-1"))
		return -EINVAL;
	property = fdt_getprop(fit, configuration, FIT_FDT_PROP, &length);
	if (!property || !nboot_update_string_equal(property, length, "fdt"))
		return -EINVAL;
	property = fdt_getprop(fit, configuration, FIT_LOADABLE_PROP, &length);
	if (!property || !length || property[length - 1])
		return -EINVAL;
	return nboot_update_check_loadables(property, length);
}

static int nboot_update_check_payloads(const void *fit,
				       struct nboot_update_image images[])
{
	const struct nboot_update_image *uboot = &images[1];
	const struct nboot_update_image *fdt = &images[2];
	const void *board_fdt = NULL;
	ulong load;
	int index;

	for (index = 0; index < NBOOT_UPDATE_IMAGE_COUNT; index++)
		if (nboot_update_check_hash(fit, images[index].node,
					    images[index].data, images[index].size))
			return -EKEYREJECTED;

	if (fit_image_get_load(fit, uboot->node, &load) ||
	    load != NBOOT_UPDATE_LOAD_ADDR ||
	    uboot->size < NBOOT_UPDATE_HEADER_SIZE ||
	    memcmp(uboot->data + 56, "ARMd", 4) ||
	    get_unaligned_le64(uboot->data + 8) != NBOOT_UPDATE_TEXT_OFFSET)
		return -ENOEXEC;
	if (fdt_check_full(fdt->data, fdt->size) ||
	    fdt_node_check_compatible(fdt->data, 0, "rockchip,rk3576"))
		return -EINVAL;

	for (index = NBOOT_UPDATE_HEADER_SIZE;
	     index + sizeof(struct fdt_header) <= uboot->size; index += 4) {
		const void *candidate = (const u8 *)uboot->data + index;
		u32 candidate_size;

		if (fdt_magic(candidate) != FDT_MAGIC || fdt_check_header(candidate))
			continue;
		candidate_size = fdt_totalsize(candidate);
		if (candidate_size > uboot->size - index)
			continue;
		if (!fdt_node_check_compatible(candidate, 0, "kickpi,k7")) {
			board_fdt = candidate;
			break;
		}
	}
	if (!board_fdt)
		return -EINVAL;

	return 0;
}

static int nboot_update_validate(const void *fit, u32 size)
{
	struct nboot_update_image images[NBOOT_UPDATE_IMAGE_COUNT] = {
		{ .node = -1 }, { .node = -1 }, { .node = -1 },
		{ .node = -1 }, { .node = -1 }, { .node = -1 },
	};
	int ret;

	if (!fit || size < sizeof(struct fdt_header) ||
	    size > NBOOT_UPDATE_FIT_SIZE)
		return -EFBIG;
	if (fdt_check_full(fit, size))
		return -EINVAL;
	ret = nboot_update_collect_images(fit, size, images);
	if (ret)
		return ret;
	ret = nboot_update_check_ranges(images);
	if (ret)
		return ret;
	ret = nboot_update_check_configuration(fit);
	if (ret)
		return ret;
	return nboot_update_check_payloads(fit, images);
}

static int nboot_update_check_layout(struct blk_desc *desc,
				     struct disk_partition *bootloader)
{
	struct disk_partition bootctrl;

	if (desc->blksz != NBOOT_UPDATE_BLOCK_SIZE ||
	    part_get_info_by_name(desc, "uboot", bootloader) < 0 ||
	    part_get_info_by_name(desc, "bootctrl", &bootctrl) < 0)
		return -EINVAL;
	if (bootloader->start != NBOOT_UPDATE_SECTOR ||
	    bootloader->size != NBOOT_UPDATE_SLOT_BLOCKS ||
	    bootctrl.start < 0x8000)
		return -EINVAL;

	return 0;
}

static int nboot_update_write_slot(struct blk_desc *desc, lbaint_t sector,
				   const void *source, void *verify)
{
	if (blk_dwrite(desc, sector, NBOOT_UPDATE_SLOT_BLOCKS, source) !=
	    NBOOT_UPDATE_SLOT_BLOCKS)
		return -EIO;
	if (blk_dread(desc, sector, NBOOT_UPDATE_SLOT_BLOCKS, verify) !=
	    NBOOT_UPDATE_SLOT_BLOCKS)
		return -EIO;
	if (memcmp(source, verify, NBOOT_UPDATE_FIT_SIZE))
		return -EIO;

	return 0;
}

int nboot_update(const void *fit, u32 size)
{
	struct disk_partition bootloader;
	struct blk_desc *desc;
	struct mmc *mmc;
	void *write_buffer;
	void *verify_buffer;
	int ret;

	if (!nboot_recovery_authorized())
		return -EPERM;
	ret = nboot_update_validate(fit, size);
	if (ret)
		return ret;

	mmc = find_mmc_device(0);
	if (!mmc || mmc_init(mmc))
		return -ENODEV;
	desc = mmc_get_blk_desc(mmc);
	ret = nboot_update_check_layout(desc, &bootloader);
	if (ret)
		return ret;

	write_buffer = memalign(ARCH_DMA_MINALIGN, NBOOT_UPDATE_FIT_SIZE);
	verify_buffer = memalign(ARCH_DMA_MINALIGN, NBOOT_UPDATE_FIT_SIZE);
	if (!write_buffer || !verify_buffer) {
		ret = -ENOMEM;
		goto out;
	}
	memset(write_buffer, 0, NBOOT_UPDATE_FIT_SIZE);
	memcpy(write_buffer, fit, size);
	flush_cache((ulong)write_buffer, NBOOT_UPDATE_FIT_SIZE);

	ret = nboot_update_write_slot(desc, bootloader.start, write_buffer,
				      verify_buffer);
out:
	free(verify_buffer);
	free(write_buffer);
	return ret;
}

// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2026 Pharos Tech */

#include <image.h>
#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <nboot_amp.h>

DECLARE_GLOBAL_DATA_PTR;

struct nboot_amp_image {
	const char *name;
	ulong load;
	u32 limit;
	u8 type;
	u8 compression;
};

static const struct nboot_amp_image nboot_amp_images[] = {
	{ "linux", NBOOT_AMP_LINUX_ENTRY, 0x05000000, IH_TYPE_KERNEL,
	  IH_COMP_NONE },
	{ "fdt", 0x4f000000, 0x01000000, IH_TYPE_FLATDT, IH_COMP_NONE },
	{ "ramdisk", 0x50000000, 0x10000000, IH_TYPE_RAMDISK, IH_COMP_GZIP },
	{ "openvela", NBOOT_AMP_NUTTX_ENTRY, 0x01000000, IH_TYPE_FIRMWARE,
	  IH_COMP_NONE },
};

static bool nboot_amp_string(const void *fdt, int node, const char *property,
			     const char *expected)
{
	int len;
	const char *value = fdt_getprop(fdt, node, property, &len);

	return value && len == strlen(expected) + 1 &&
	       !memcmp(value, expected, len);
}

static bool nboot_amp_u32(const void *fdt, int node, const char *property,
			  u32 expected)
{
	int len;
	const fdt32_t *value = fdt_getprop(fdt, node, property, &len);

	return value && len == sizeof(*value) &&
	       fdt32_to_cpu(*value) == expected;
}

static int nboot_amp_check_cpus(const void *fdt)
{
	int cpus = fdt_path_offset(fdt, "/cpus");
	int cells, node;
	u32 found = 0;

	if (cpus < 0)
		return -EINVAL;
	cells = fdt_address_cells(fdt, cpus);
	if (cells != 1 && cells != 2)
		return -EINVAL;
	fdt_for_each_subnode(node, fdt, cpus) {
		const fdt32_t *reg;
		const char *name = fdt_get_name(fdt, node, NULL);
		unsigned int cpu;
		u64 affinity;
		int len;

		if (!nboot_amp_string(fdt, node, "device_type", "cpu")) {
			if (!strcmp(name, "cpu") || !strncmp(name, "cpu@", 4))
				return -EINVAL;
			continue;
		}
		if (!nboot_amp_string(fdt, node, "enable-method", "psci") ||
		    fdt_node_check_compatible(fdt, node, "arm,cortex-a72"))
			return -EINVAL;
		reg = fdt_getprop(fdt, node, "reg", &len);
		if (!reg || len != cells * sizeof(*reg))
			return -EINVAL;
		affinity = fdt32_to_cpu(reg[0]);
		if (cells == 2)
			affinity = affinity << 32 | fdt32_to_cpu(reg[1]);
		if (affinity >= 0x100 && affinity <= 0x103) {
			cpu = affinity - 0x100;
			if (fdt_getprop(fdt, node, "status", NULL) &&
			    !nboot_amp_string(fdt, node, "status", "okay") &&
			    !nboot_amp_string(fdt, node, "status", "ok"))
				return -EINVAL;
		} else {
			return -EINVAL;
		}
		if (found & BIT(cpu))
			return -EINVAL;
		found |= BIT(cpu);
	}
	return found == 0x0f ? 0 : -EINVAL;
}

int nboot_amp_validate_fdt(const void *fdt, u32 size)
{
	static const struct {
		const char *path;
		u32 base;
		u32 size;
	} regions[] = {
		{ "/reserved-memory/rpmsg@47800000", 0x47800000, 0x200000 },
		{ "/reserved-memory/rpmsg-dma@47a00000", 0x47a00000, 0x200000 },
		{ "/reserved-memory/amp-shmem@47c00000", 0x47c00000, 0x400000 },
		{ "/reserved-memory/openvela@4a400000", 0x4a400000, 0x1000000 },
	};
	int i, parent, amp, uart, len;
	const fdt32_t *routes;
	u32 found = 0;

	if (fdt_check_full(fdt, size) || nboot_amp_check_cpus(fdt))
		return -EINVAL;
	parent = fdt_path_offset(fdt, "/reserved-memory");
	if (parent < 0 || fdt_address_cells(fdt, parent) != 2 ||
	    fdt_size_cells(fdt, parent) != 2 ||
	    !fdt_getprop(fdt, parent, "ranges", &len) || len)
		return -EINVAL;
	uart = fdt_path_offset(fdt, "/serial@2ad40000");
	if (uart < 0 || !nboot_amp_string(fdt, uart, "status", "disabled"))
		return -EINVAL;
	amp = fdt_node_offset_by_compatible(fdt, -1, "rockchip,fiq-debugger");
	while (amp >= 0) {
		if (!nboot_amp_string(fdt, amp, "status", "disabled"))
			return -EINVAL;
		amp = fdt_node_offset_by_compatible(fdt, amp,
						    "rockchip,fiq-debugger");
	}
	amp = fdt_node_offset_by_compatible(fdt, -1, "rockchip,amp");
	if (amp < 0 || !nboot_amp_string(fdt, amp, "status", "okay") ||
	    fdt_node_offset_by_compatible(fdt, amp, "rockchip,amp") >= 0)
		return -EINVAL;
	routes = fdt_getprop(fdt, amp, "amp-irqs", &len);
	if (!routes || len != 12 * sizeof(*routes))
		return -EINVAL;
	for (i = 0; i < 12; i += 6) {
		u32 irq = fdt32_to_cpu(routes[i + 1]);

		if (routes[i] || fdt32_to_cpu(routes[i + 2]) ||
		    fdt32_to_cpu(routes[i + 3]) != NBOOT_AMP_IRQ_PRIORITY ||
		    routes[i + 4] || routes[i + 5])
			return -EINVAL;
		if (irq == NBOOT_AMP_UART_IRQ)
			found |= 1;
		else if (irq == NBOOT_AMP_MBOX_IRQ)
			found |= 2;
		else
			return -EINVAL;
	}
	if (found != 3)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		int node = fdt_path_offset(fdt, regions[i].path);
		int len;
		const fdt32_t *reg;

		if (node < 0 || !fdt_getprop(fdt, node, "no-map", NULL))
			return -EINVAL;
		reg = fdt_getprop(fdt, node, "reg", &len);
		if (!reg || len != 4 * sizeof(*reg) || reg[0] || reg[2] ||
		    fdt32_to_cpu(reg[1]) != regions[i].base ||
		    fdt32_to_cpu(reg[3]) != regions[i].size)
			return -EINVAL;
	}
	return 0;
}

int nboot_amp_validate(const void *fit, u32 size)
{
	u32 positions[ARRAY_SIZE(nboot_amp_images)];
	u32 sizes[ARRAY_SIZE(nboot_amp_images)];
	int images_node, conf, node, count = 0, i, j;

	if (!fit || size < sizeof(struct fdt_header) ||
	    size > NBOOT_AMP_MAX_FIT_SIZE || fdt_check_full(fit, size))
		return -EINVAL;
	conf = fdt_path_offset(fit, "/configurations/conf");
	images_node = fdt_path_offset(fit, "/images");
	if (conf < 0 || images_node < 0 ||
	    !nboot_amp_u32(fit, conf, "nyabula,amp-abi",
			   NBOOT_AMP_ABI_VERSION) ||
	    !nboot_amp_string(fit, conf, "kernel", "linux") ||
	    !nboot_amp_string(fit, conf, "fdt", "fdt") ||
	    !nboot_amp_string(fit, conf, "ramdisk", "ramdisk") ||
	    !nboot_amp_string(fit, conf, "loadables", "openvela") ||
	    fdt_getprop(fit, conf, "firmware", NULL))
		return -EINVAL;
	fdt_for_each_subnode(node, fit, images_node)
		count++;
	if (count != ARRAY_SIZE(nboot_amp_images))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(nboot_amp_images); i++) {
		const struct nboot_amp_image *image = &nboot_amp_images[i];
		const u8 *data;
		int position, length, hash;
		ulong load, entry;
		bool sha256 = false;

		node = fdt_subnode_offset(fit, images_node, image->name);
		if (node < 0 ||
		    fit_image_get_data_position(fit, node, &position) ||
		    fit_image_get_data_size(fit, node, &length) ||
		    position < 0 || length <= 0 ||
		    (u32)position < fdt_totalsize(fit) || (position & 7) ||
		    (u32)position > size || (u32)length > size - position ||
		    (u32)length > image->limit ||
		    fdt_getprop(fit, node, "data", NULL) ||
		    fdt_getprop(fit, node, "data-offset", NULL) ||
		    fit_image_get_load(fit, node, &load) ||
		    load != image->load ||
		    !fit_image_check_type(fit, node, image->type) ||
		    !fit_image_check_arch(fit, node, IH_ARCH_ARM64) ||
		    !fit_image_check_comp(fit, node, image->compression))
			return -EINVAL;
		positions[i] = position;
		sizes[i] = length;
		for (j = 0; j < i; j++)
			if (positions[i] < positions[j] + sizes[j] &&
			    positions[j] < positions[i] + sizes[i])
				return -EINVAL;
		data = (const u8 *)fit + position;
		if (i == 0 || i == 3) {
			u64 runtime_size, text_offset;

			if (fit_image_get_entry(fit, node, &entry) ||
			    entry != load ||
			    !nboot_amp_u32(fit, node, "cpu",
					   i ? 0 : NBOOT_AMP_LINUX_CPU) ||
			    length < 64 || memcmp(data + 56, "ARMd", 4))
				return -EINVAL;
			runtime_size = get_unaligned_le64(data + 16);
			if (runtime_size < (u32)length ||
			    runtime_size > image->limit ||
			    (get_unaligned_le64(data + 24) & 1))
				return -EINVAL;
			if (i == 0) {
				text_offset = get_unaligned_le64(data + 8);
				if (text_offset > load ||
				    ((load - text_offset) & 0x1fffff))
					return -EINVAL;
			}
		}
		if ((i == 0 || i == 2) &&
		    !nboot_amp_string(fit, node, "os", "linux"))
			return -EINVAL;
		fdt_for_each_subnode(hash, fit, node) {
			int len;
			const char *name = fdt_get_name(fit, hash, NULL);

			if (strncmp(name, FIT_HASH_NODENAME,
				    strlen(FIT_HASH_NODENAME)) ||
			    !nboot_amp_string(fit, hash, "algo", "sha256"))
				continue;
			if (fdt_getprop(fit, hash, "ignore", NULL) ||
			    !fdt_getprop(fit, hash, "value", &len) || len != 32)
				return -EKEYREJECTED;
			sha256 = true;
		}
		if (!sha256 || !fit_image_verify_with_data(
				       fit, node, gd_fdt_blob(), data, length))
			return -EKEYREJECTED;
		if (i == 1 && nboot_amp_validate_fdt(data, length))
			return -EINVAL;
	}
	return 0;
}

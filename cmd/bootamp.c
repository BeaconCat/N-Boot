// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2026 Pharos Tech */

#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <bootm.h>
#include <command.h>
#include <cpu_func.h>
#include <dm.h>
#include <env.h>
#include <hang.h>
#include <image.h>
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/psci.h>
#include <lmb.h>
#include <mapmem.h>
#include <malloc.h>
#include <nboot_amp.h>
#include <time.h>
#include <virtio_ring.h>

DECLARE_GLOBAL_DATA_PTR;

#define NBOOT_AMP_GICD 0x2a701000UL
#define NBOOT_AMP_GIC_ENABLE 0x000
#define NBOOT_AMP_GIC_TYPER 0x004
#define NBOOT_AMP_GIC_PRIORITY 0x400
#define NBOOT_AMP_GIC_TARGET 0x800
#define NBOOT_AMP_GIC_WAIT_MS 30000
#define NBOOT_AMP_GIC_READY_WORD 0xa0a0a0a0U
#define NBOOT_AMP_SIP 0x82000022UL
#define NBOOT_AMP_SIP_PE_STATE 0
#define NBOOT_AMP_SIP_ARGS01 1
#define NBOOT_AMP_SIP_ARGS23 2
#define NBOOT_AMP_PE_A64_EL2_NS 0x16
#define NBOOT_AMP_MBOX0 0x2ae50000UL
#define NBOOT_AMP_MBOX3 0x2ae53000UL
#define NBOOT_AMP_MBOX_A2B_INTEN 0x00
#define NBOOT_AMP_MBOX_RX_ENABLE 0x00010001U
#define NBOOT_AMP_MBOX_A2B_STATUS 0x04
#define NBOOT_AMP_MBOX_A2B_CMD 0x08
#define NBOOT_AMP_MBOX_A2B_DATA 0x0c
#define NBOOT_AMP_MBOX_B2A_STATUS 0x14
#define NBOOT_AMP_MBOX_GATE 0x27200844UL
#define NBOOT_AMP_MBOX_GATE_ON (1U << 29)
#define NBOOT_AMP_MBOX_LINK 0x03
#define NBOOT_AMP_MBOX_MAGIC 0x524d5347

static bool nboot_amp_dram_range(ulong base, ulong size)
{
	int bank;

	if (!size || base + size < base)
		return false;
	for (bank = 0; bank < CONFIG_NR_DRAM_BANKS; bank++)
		if (base >= gd->dram[bank].start &&
		    base - gd->dram[bank].start <= gd->dram[bank].size &&
		    size <= gd->dram[bank].size - (base - gd->dram[bank].start))
			return true;
	return false;
}

static int nboot_amp_cpus_off(void)
{
	static const ulong cpus[] = { 1, 2, 3, 0x100, 0x101, 0x102, 0x103 };
	struct udevice *dev;
	int i, ret;

	if ((read_mpidr() & 0xff00ffffffULL) != 0)
		return -EINVAL;
	ret = uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &dev);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(cpus); i++) {
		ret = (long)invoke_psci_fn(PSCI_0_2_FN64_AFFINITY_INFO, cpus[i],
					   0, 0);
		if (ret < 0)
			return ret;
		if (ret != PSCI_0_2_AFFINITY_LEVEL_OFF)
			return -EBUSY;
	}
	return 0;
}

static int nboot_amp_states(int states, const char *spec)
{
	struct bootm_info bmi;

	bootm_init(&bmi);
	bmi.cmd_name = "bootamp";
	bmi.addr_img = spec;
	return bootm_run_states(&bmi, states);
}

static int nboot_amp_sip(ulong operation, ulong first, ulong second)
{
	struct arm_smccc_res result;

	arm_smccc_smc(NBOOT_AMP_SIP, operation, NBOOT_AMP_LINUX_CPU, first,
		      second, 0, 0, 0, &result);
	return (int)result.a0;
}

static void nboot_amp_reset(const char *reason)
{
	printf("bootamp: %s; resetting the whole SoC\n", reason);
	reset_cpu();
	hang();
}

static bool nboot_amp_resident_reserved(const void *fdt, ulong start, ulong end)
{
	int i, count = fdt_num_mem_rsv(fdt);
	uint64_t base, size;

	for (i = 0; i < count; i++)
		if (!fdt_get_mem_rsv(fdt, i, &base, &size) && base == start &&
		    size == end - start)
			return true;
	return false;
}

static bool nboot_amp_irq_ready(unsigned int irq, u8 target)
{
	ulong target_reg = NBOOT_AMP_GICD + NBOOT_AMP_GIC_TARGET + irq;
	ulong priority_reg = NBOOT_AMP_GICD + NBOOT_AMP_GIC_PRIORITY + irq;

	return readb(target_reg) == target &&
	       readb(priority_reg) == NBOOT_AMP_IRQ_PRIORITY;
}

static int nboot_amp_reserve_load_windows(void)
{
	static const struct {
		phys_addr_t base;
		phys_size_t size;
		u32 flags;
	} windows[] = {
		{ NBOOT_AMP_LINUX_ENTRY, 0x05000000, LMB_NONE },
		{ 0x47800000, 0x00800000, LMB_NOMAP },
		{ NBOOT_AMP_NUTTX_ENTRY, 0x01000000, LMB_NOMAP },
		{ 0x4f000000, 0x01000000, LMB_NONE },
		{ 0x50000000, 0x10000000, LMB_NONE },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(windows); i++) {
		phys_addr_t base = windows[i].base;

		ret = lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &base,
				    windows[i].size, windows[i].flags);
		if (ret)
			return ret;
	}
	return 0;
}

static bool nboot_amp_receive_buffers_ready(void)
{
	const struct vring_desc *descs = (const void *)NBOOT_AMP_VRING0_BASE;
	const struct vring_avail *avail =
		(const void *)(descs + NBOOT_AMP_VRING_NUM);
	u64 first = readq((ulong)&descs[0].addr);
	unsigned int i;

	if (readw((ulong)&avail->idx) != NBOOT_AMP_VRING_NUM ||
	    first < NBOOT_AMP_BUFFER_BASE ||
	    first > NBOOT_AMP_BUFFER_BASE + NBOOT_AMP_BUFFER_LIMIT -
			    NBOOT_AMP_VRING_NUM * NBOOT_AMP_BUFFER_SIZE)
		return false;
	for (i = 0; i < NBOOT_AMP_VRING_NUM; i++)
		if (readq((ulong)&descs[i].addr) !=
			    first + i * NBOOT_AMP_BUFFER_SIZE ||
		    readl((ulong)&descs[i].len) != NBOOT_AMP_BUFFER_SIZE ||
		    readw((ulong)&descs[i].flags) != VRING_DESC_F_WRITE)
			return false;
	return true;
}

static int do_bootamp(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	ulong address, size, resident_start, resident_end, started;
	ulong marker, fdt_address;
	u32 lines, value;
	u8 target;
	char spec[40];
	void *fdt;
	int ret;
	bool check;

	if ((argc != 3 && argc != 4) || strict_strtoul(argv[1], 16, &address) ||
	    strict_strtoul(argv[2], 16, &size) ||
	    (argc == 4 && strcmp(argv[3], "check")))
		return CMD_RET_USAGE;
	check = argc == 4;
	if (size > NBOOT_AMP_MAX_FIT_SIZE ||
	    !nboot_amp_dram_range(address, size) ||
	    gd->start_addr_sp < CONFIG_STACK_SIZE ||
	    (gd->flags & GD_FLG_SKIP_RELOC))
		return CMD_RET_FAILURE;
	resident_start = gd->start_addr_sp - CONFIG_STACK_SIZE;
	resident_end = gd->initial_relocaddr;
	if (resident_end <= resident_start ||
	    !nboot_amp_dram_range(resident_start,
				  resident_end - resident_start) ||
	    !nboot_amp_dram_range(NBOOT_AMP_LINUX_ENTRY,
				  0x60000000UL - NBOOT_AMP_LINUX_ENTRY) ||
	    (resident_start < 0x60000000UL &&
	     resident_end > NBOOT_AMP_LINUX_ENTRY) ||
	    (address < resident_end && resident_start < address + size))
		return CMD_RET_FAILURE;

	/* Keep staging away from all fixed load windows, even unused padding. */

	if (address < 0x60000000UL && address + size > NBOOT_AMP_LINUX_ENTRY)
		return CMD_RET_FAILURE;
	ret = nboot_amp_validate((void *)address, size);
	if (ret) {
		printf("bootamp: FIT rejected before loading (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	if (check) {
		puts("bootamp: FIT preflight passed; no CPU or payload changed\n");
		return CMD_RET_SUCCESS;
	}
	ret = nboot_amp_cpus_off();
	if (ret) {
		printf("bootamp: cold CPU topology required (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	if (env_get("fdt_high") || env_get("initrd_high")) {
		puts("bootamp: unset fdt_high and initrd_high for bounded relocation\n");
		return CMD_RET_FAILURE;
	}
	{
		phys_addr_t staging = address;

		ret = lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, 0, &staging, size,
				    LMB_NONE);
		if (ret) {
			printf("bootamp: cannot reserve FIT staging (%d)\n",
			       ret);
			return CMD_RET_FAILURE;
		}
	}
	snprintf(spec, sizeof(spec), "%lx#conf", address);
	ret = nboot_amp_states(BOOTM_STATE_START | BOOTM_STATE_PRE_LOAD |
				       BOOTM_STATE_FINDOS |
				       BOOTM_STATE_FINDOTHER,
			       spec);
	if (ret)
		return CMD_RET_FAILURE;
	ret = nboot_amp_reserve_load_windows();
	if (ret) {
		printf("bootamp: cannot protect load windows (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	ret = nboot_amp_states(BOOTM_STATE_MEASURE | BOOTM_STATE_LOADOS |
				       BOOTM_STATE_RAMDISK | BOOTM_STATE_FDT,
			       NULL);
	if (ret)
		return CMD_RET_FAILURE;
	fdt = (void *)images.ft_addr;
	ret = fdt_add_mem_rsv(fdt, resident_start,
			      resident_end - resident_start);
	if (ret) {
		printf("bootamp: cannot reserve resident bootloader (%d)\n",
		       ret);
		return CMD_RET_FAILURE;
	}
	{
		const char *old = env_get("bootargs");
		char *saved = old ? strdup(old) : NULL;
		int restore;

		if (old && !saved)
			return CMD_RET_FAILURE;
		ret = env_set("bootargs",
			      "rdinit=/init console=ttynull clk_ignore_unused "
			      "pd_ignore_unused cpuidle.off=1");
		if (!ret)
			ret = nboot_amp_states(BOOTM_STATE_OS_PREP, NULL);
		restore = env_set("bootargs", saved);
		free(saved);
		if (ret || restore)
			return CMD_RET_FAILURE;
	}
	fdt = (void *)images.ft_addr;
	fdt_address = map_to_sysmem(images.ft_addr);
	if (images.ep != NBOOT_AMP_LINUX_ENTRY || fdt_address > 0xffffffffUL ||
	    nboot_amp_validate_fdt(fdt, fdt_totalsize(fdt)) ||
	    !nboot_amp_resident_reserved(fdt, resident_start, resident_end))
		return CMD_RET_FAILURE;
	ret = fdt_path_offset(fdt, "/chosen");
	if (ret < 0)
		return CMD_RET_FAILURE;
	fdt_delprop(fdt, ret, "stdout-path");
	fdt_delprop(fdt, ret, "linux,stdout-path");
	fdt_set_boot_cpuid_phys(fdt, NBOOT_AMP_LINUX_CPU);

	lines = 32 * ((readl(NBOOT_AMP_GICD + NBOOT_AMP_GIC_TYPER) & 31) + 1);
	lines = min(lines, 1020U);
	if (lines <= NBOOT_AMP_MBOX_IRQ + 2)
		return CMD_RET_FAILURE;
	marker = NBOOT_AMP_GICD + NBOOT_AMP_GIC_PRIORITY + lines - 4;
	target = readb(NBOOT_AMP_GICD + NBOOT_AMP_GIC_TARGET);
	if (!target || (target & (target - 1)))
		return CMD_RET_FAILURE;

	/* Vendor BL31 provides nonboot-CPU Linux argument and EL configuration. */

	if (nboot_amp_sip(NBOOT_AMP_SIP_PE_STATE, NBOOT_AMP_PE_A64_EL2_NS, 0) ||
	    nboot_amp_sip(NBOOT_AMP_SIP_ARGS01, fdt_address, 0) ||
	    nboot_amp_sip(NBOOT_AMP_SIP_ARGS23, 0, 0))
		nboot_amp_reset("BL31 AMP argument service unavailable");
	puts("bootamp: launching Linux on 0x100; NuttX waits on CPU0\n");

	/* Quiesce devices and flush before the other kernel can start DMA. */

	bootm_disable_interrupts();
	bootm_final(0);
	cleanup_before_linux();
	value = readl(NBOOT_AMP_GICD + NBOOT_AMP_GIC_ENABLE);
	writel(value & ~3U, NBOOT_AMP_GICD + NBOOT_AMP_GIC_ENABLE);
	writel(0, marker);

	/* Both domains are still off: discard only the previous boot's kicks. */

	writel(NBOOT_AMP_MBOX_GATE_ON, NBOOT_AMP_MBOX_GATE);
	writel(1, NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_STATUS);
	writel(1, NBOOT_AMP_MBOX0 + NBOOT_AMP_MBOX_B2A_STATUS);
	writel(1, NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_B2A_STATUS);

	/* Status is only latched with the receive interrupt enabled.  CPU0 IRQs
	 * remain masked until NuttX installs its mailbox handler.
	 */

	writel(NBOOT_AMP_MBOX_RX_ENABLE,
	       NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_INTEN);
	dsb();
	if ((readl(NBOOT_AMP_GICD + NBOOT_AMP_GIC_ENABLE) & 3) ||
	    readl(marker) ||
	    (readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_STATUS) & 1) ||
	    (readl(NBOOT_AMP_MBOX0 + NBOOT_AMP_MBOX_B2A_STATUS) & 1) ||
	    (readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_B2A_STATUS) & 1))
		nboot_amp_reset("cannot arm fresh GIC handshake");
	ret = (long)invoke_psci_fn(PSCI_0_2_FN64_CPU_ON, NBOOT_AMP_LINUX_CPU,
				   images.ep, 0);
	if (ret != PSCI_RET_SUCCESS)
		nboot_amp_reset("Linux CPU_ON failed");
	started = get_timer(0);
	for (;;) {
		if (readl(marker) == NBOOT_AMP_GIC_READY_WORD &&
		    (readl(NBOOT_AMP_GICD + NBOOT_AMP_GIC_ENABLE) & 1) &&
		    nboot_amp_irq_ready(NBOOT_AMP_UART_IRQ, target) &&
		    nboot_amp_irq_ready(NBOOT_AMP_MBOX_IRQ, target) &&
		    (readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_STATUS) & 1) &&
		    (readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_CMD) & 0xff) ==
			    NBOOT_AMP_MBOX_LINK &&
		    readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_DATA) ==
			    NBOOT_AMP_MBOX_MAGIC) {
			if (!nboot_amp_receive_buffers_ready())
				nboot_amp_reset(
					"Linux RPMsg buffer contract mismatch");
			break;
		}
		if (get_timer(started) >= NBOOT_AMP_GIC_WAIT_MS) {
			printf("bootamp: GIC marker=%08x control=%08x target=%02x\n",
			       readl(marker), readl(NBOOT_AMP_GICD), target);
			printf("bootamp: UART route=%02x/%02x mailbox route=%02x/%02x\n",
			       readb(NBOOT_AMP_GICD + NBOOT_AMP_GIC_TARGET +
				     NBOOT_AMP_UART_IRQ),
			       readb(NBOOT_AMP_GICD + NBOOT_AMP_GIC_PRIORITY +
				     NBOOT_AMP_UART_IRQ),
			       readb(NBOOT_AMP_GICD + NBOOT_AMP_GIC_TARGET +
				     NBOOT_AMP_MBOX_IRQ),
			       readb(NBOOT_AMP_GICD + NBOOT_AMP_GIC_PRIORITY +
				     NBOOT_AMP_MBOX_IRQ));
			printf("bootamp: mailbox status=%08x cmd=%08x data=%08x\n",
			       readl(NBOOT_AMP_MBOX3 +
				     NBOOT_AMP_MBOX_A2B_STATUS),
			       readl(NBOOT_AMP_MBOX3 + NBOOT_AMP_MBOX_A2B_CMD),
			       readl(NBOOT_AMP_MBOX3 +
				     NBOOT_AMP_MBOX_A2B_DATA));
			nboot_amp_reset("Linux GIC/RPMsg readiness timed out");
		}
		udelay(10);
	}
	dsb();
	((void (*)(void))NBOOT_AMP_NUTTX_ENTRY)();
	nboot_amp_reset("NuttX entry returned");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(bootamp, 4, 0, do_bootamp,
	   "cold-boot a validated four-A53/four-A72 AMP FIT",
	   "<fit-address> <fit-size> [check]\n"
	   "    - hexadecimal address/size; check validates without loading");

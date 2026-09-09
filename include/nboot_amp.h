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
#define NBOOT_AMP_VRING0_BASE 0x47800000UL
#define NBOOT_AMP_VRING_NUM 64
#define NBOOT_AMP_BUFFER_BASE 0x47a00000UL
#define NBOOT_AMP_BUFFER_LIMIT 0x00200000UL
#define NBOOT_AMP_BUFFER_SIZE 512

/* Validate the complete external-data FIT before any payload is loaded. */
int nboot_amp_validate(const void *fit, u32 size);
int nboot_amp_validate_fdt(const void *fdt, u32 size);

#endif

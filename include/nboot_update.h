/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * N-Boot verified redundant FIT update interface.
 *
 * Copyright 2026 Pharos Tech
 */

#ifndef __NBOOT_UPDATE_H
#define __NBOOT_UPDATE_H

#include <linux/types.h>

/**
 * nboot_update() - verify and install a KICKPI-K7 N-Boot FIT image
 * @fit: complete external-data FIT image in memory
 * @size: size of @fit in bytes
 *
 * Verifies the complete candidate before changing media, then updates the
 * vendor-compatible 4 MiB SD bootloader region with a full readback check.
 *
 * Return: 0 on success or a negative errno value
 */
int nboot_update(const void *fit, u32 size);

#endif /* __NBOOT_UPDATE_H */

// SPDX-License-Identifier: GPL-2.0+
/*
 * N-Boot Fastboot recovery session state.
 *
 * Copyright 2026 Pharos Tech
 */

#include <nboot_recovery.h>
#include <nboot_storage.h>

void nboot_recovery_reset(void)
{
	nboot_storage_reset_target();
}

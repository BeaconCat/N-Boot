/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * N-Boot Fastboot recovery session authorization.
 *
 * Copyright 2026 Pharos Tech
 */

#ifndef __NBOOT_RECOVERY_H
#define __NBOOT_RECOVERY_H

#include <stdbool.h>

void nboot_recovery_reset(void);
bool nboot_recovery_authorized(void);
bool nboot_recovery_unlock(const char *parameter, char *response);

#endif /* __NBOOT_RECOVERY_H */

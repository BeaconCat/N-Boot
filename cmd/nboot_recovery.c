// SPDX-License-Identifier: GPL-2.0+
/*
 * Fastboot recovery session authorization for N-Boot.
 *
 * Copyright 2026 Pharos Tech
 */

#include <linux/string.h>
#include <dm.h>
#include <env.h>
#include <fastboot.h>
#include <nboot_recovery.h>
#include <rng.h>
#include <time.h>

#define NBOOT_RECOVERY_CHALLENGE_BYTES	8
#define NBOOT_RECOVERY_CHALLENGE_TIMEOUT	60000
#define NBOOT_RECOVERY_UNLOCK_TIMEOUT	120000

static u8 challenge[NBOOT_RECOVERY_CHALLENGE_BYTES];
static ulong challenge_start;
static ulong unlock_start;
static bool challenge_pending;
static bool session_authorized;

static void nboot_recovery_hex(char *output)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < NBOOT_RECOVERY_CHALLENGE_BYTES; i++) {
		output[i * 2] = hex[challenge[i] >> 4];
		output[i * 2 + 1] = hex[challenge[i] & 0xf];
	}

	output[NBOOT_RECOVERY_CHALLENGE_BYTES * 2] = '\0';
}

static bool nboot_recovery_match(const char *parameter)
{
	char expected[NBOOT_RECOVERY_CHALLENGE_BYTES * 2 + 1];
	unsigned int mismatch = 0;
	int i;

	if (strlen(parameter) != NBOOT_RECOVERY_CHALLENGE_BYTES * 2)
		return false;

	nboot_recovery_hex(expected);
	for (i = 0; i < NBOOT_RECOVERY_CHALLENGE_BYTES * 2; i++)
		mismatch |= (unsigned char)parameter[i] ^ (unsigned char)expected[i];

	return !mismatch;
}

void nboot_recovery_reset(void)
{
	env_set("fastboot.nboot-challenge", NULL);
	memset(challenge, 0, sizeof(challenge));
	challenge_start = 0;
	unlock_start = 0;
	challenge_pending = false;
	session_authorized = false;
}

bool nboot_recovery_authorized(void)
{
	if (session_authorized &&
	    get_timer(unlock_start) >= NBOOT_RECOVERY_UNLOCK_TIMEOUT)
		nboot_recovery_reset();

	return session_authorized;
}

bool nboot_recovery_unlock(const char *parameter, char *response)
{
	struct udevice *dev;
	char confirmation[NBOOT_RECOVERY_CHALLENGE_BYTES * 2 + 1];
	int ret;

	if (!parameter)
		return false;

	if (!strcmp(parameter, "lock")) {
		nboot_recovery_reset();
		fastboot_okay("advanced session locked", response);
		return true;
	}

	if (!strcmp(parameter, "unlock-request")) {
		nboot_recovery_reset();
		ret = uclass_get_device(UCLASS_RNG, 0, &dev);
		if (ret || !dev || dm_rng_read(dev, challenge, sizeof(challenge))) {
			nboot_recovery_reset();
			fastboot_fail("hardware RNG unavailable", response);
			return true;
		}

		challenge_start = get_timer(0);
		challenge_pending = true;
		nboot_recovery_hex(confirmation);
		env_set("fastboot.nboot-challenge", confirmation);
		fastboot_okay(confirmation, response);
		return true;
	}

	if (strncmp(parameter, "unlock-confirm:", 15))
		return false;

	if (!challenge_pending ||
	    get_timer(challenge_start) >= NBOOT_RECOVERY_CHALLENGE_TIMEOUT) {
		nboot_recovery_reset();
		fastboot_fail("unlock request expired", response);
		return true;
	}

	if (!nboot_recovery_match(parameter + 15)) {
		nboot_recovery_reset();
		fastboot_fail("invalid unlock confirmation", response);
		return true;
	}

	memset(challenge, 0, sizeof(challenge));
	env_set("fastboot.nboot-challenge", NULL);
	challenge_pending = false;
	session_authorized = true;
	unlock_start = get_timer(0);
	fastboot_okay("advanced session unlocked", response);
	return true;
}

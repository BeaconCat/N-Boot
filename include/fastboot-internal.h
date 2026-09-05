/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _FASTBOOT_INTERNAL_H_
#define _FASTBOOT_INTERNAL_H_

#include <linux/types.h>

/**
 * fastboot_buf_addr - base address of the fastboot download buffer
 */
extern void *fastboot_buf_addr;

/**
 * fastboot_buf_size - size of the fastboot download buffer
 */
extern u32 fastboot_buf_size;

/**
 * fastboot_progress_callback - callback executed during long operations
 */
extern void (*fastboot_progress_callback)(const char *msg);

/**
 * fastboot_getvar_all() - Writes current variable being listed from "all" to response.
 *
 * @response: Pointer to fastboot response buffer
 */
void fastboot_getvar_all(char *response);

/**
 * fastboot_getvar() - Writes variable indicated by cmd_parameter to response.
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 *
 * Look up cmd_parameter first as an environment variable of the form
 * fastboot.<cmd_parameter>, if that exists return use its value to set
 * response.
 *
 * Otherwise lookup the name of variable and execute the appropriate
 * function to return the requested value.
 */
void fastboot_getvar(char *cmd_parameter, char *response);

/**
 * fastboot_oem_board() - run a board-owned Fastboot operation
 * @parameter: board operation and arguments
 * @data: staged download buffer
 * @size: staged download size
 * @response: Fastboot response buffer
 */
void fastboot_oem_board(char *parameter, void *data, u32 size,
			char *response);

#endif

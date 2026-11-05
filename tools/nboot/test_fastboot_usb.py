#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Exercise the real fastboot USB lifecycle without accessing a USB device."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / 'cmd/fastboot.c').read_text()
start = source.index('static int do_fastboot_usb(')
end = source.index('\n}\n', start) + 3
function = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define IS_ENABLED(x) (x)
#define CONFIG_USB_FUNCTION_FASTBOOT 1
#define CONFIG_CMD_FASTBOOT_ABORT_KEYED 0
#define CMD_RET_USAGE 2
#define CMD_RET_FAILURE 1
#define CMD_RET_SUCCESS 0
#define pr_err printf
struct udevice { int unused; } controller;
static int get_error, register_error, gets, registrations, unregisters, puts_count;
static bool registered, cable;
static unsigned events;
unsigned long simple_strtoul(const char *s, char **end, unsigned int base)
{ return strtoul(s, end, base); }
int udc_device_get_by_index(int index, struct udevice **out)
{ assert(index == 0); gets++; *out = &controller; return get_error; }
void udc_device_put(struct udevice *udc)
{ assert(udc == &controller && !registered); puts_count++; events = events * 10 + 2; }
void g_dnl_clear_detach(void) {}
int g_dnl_register(const char *name)
{ (void)name; registrations++; registered = !register_error; return register_error; }
void g_dnl_unregister(void)
{ assert(registered); registered = false; unregisters++; events = events * 10 + 1; }
int g_dnl_board_usb_cable_connected(void) { return cable; }
int g_dnl_detach(void) { return 1; }
int tstc(void) { return 0; }
int ctrlc(void) { return 1; }
void schedule(void) {}
void dm_usb_gadget_handle_interrupts(struct udevice *udc) { (void)udc; }
''' + function + r'''
static void reset(void)
{
 get_error = register_error = gets = registrations = unregisters = puts_count = 0;
 registered = false; cable = true; events = 0;
}
int main(void)
{
 char *args[] = {"usb", "0"};
 reset();
 assert(do_fastboot_usb(1, args, 0, 0) == CMD_RET_USAGE && !gets);
 reset(); get_error = -5;
 assert(do_fastboot_usb(2, args, 0, 0) == CMD_RET_FAILURE);
 assert(gets == 1 && !registrations && !puts_count);
 reset();
 assert(do_fastboot_usb(2, args, 0, 0) == CMD_RET_SUCCESS);
 assert(unregisters == 1 && puts_count == 1 && events == 12);
 reset(); cable = false;
 assert(do_fastboot_usb(2, args, 0, 0) == CMD_RET_FAILURE);
 assert(unregisters == 1 && puts_count == 1 && events == 12);
 reset(); register_error = -12;
 assert(do_fastboot_usb(2, args, 0, 0) == -12);
 assert(gets == 1 && registrations == 1 && !unregisters && puts_count == 1);
 assert(events == 2);
 puts("FASTBOOT_USB_LIFECYCLE_PASS ordering cable_failure acquisition_failure registration_failure");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nboot-fastboot-usb-') as directory:
    root = Path(directory)
    (root / 'test.c').write_text(fixture)
    subprocess.run(['cc', '-Wall', '-Werror', str(root / 'test.c'),
                    '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)

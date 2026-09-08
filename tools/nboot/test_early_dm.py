# SPDX-License-Identifier: GPL-2.0+
"""Host regression for the actual minimal pre-relocation board hook."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = (Path(sys.argv[1]) if len(sys.argv) > 1 else
        Path(__file__).resolve().parents[2])
source = (root / 'arch/arm/mach-rockchip/rk3576/rk3576.c').read_text()
start = source.index('int board_early_init_f(void)')
end = source.index('\n}\n', start) + 3
hook = source[start:end]
test = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef int ofnode;
struct udevice { int dummy; } root;
struct global_data { struct udevice *dm_root; } data = { &root };
static struct global_data *gd = &data;
#define DECLARE_GLOBAL_DATA_PTR
static int valid = 1, called, result;
static ofnode ofnode_path(const char *path)
{ assert(strcmp(path, "/dmc") == 0); return valid; }
static bool ofnode_valid(ofnode node) { return node != 0; }
static int lists_bind_fdt(struct udevice *parent, ofnode node,
                         struct udevice **dev, void *driver, bool pre)
{ assert(parent == &root && node == 1 && dev && !driver && pre);
  called++; return result; }
''' + hook + r'''
int main(void)
{
  assert(board_early_init_f() == 0 && called == 1);
  result = -ENOMEM;
  assert(board_early_init_f() == -ENOMEM && called == 2);
  valid = 0;
  assert(board_early_init_f() == -ENODEV && called == 2);
  return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    c = Path(folder) / 'test.c'
    binary = Path(folder) / 'test'
    c.write_text(test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
for name in ['kickpi-k7-rk3576_defconfig', 'kickpi-k7-rk3576-full_defconfig']:
    config = (root / 'configs' / name).read_text()
    for required in ['CONFIG_SKIP_EARLY_DM=y', 'CONFIG_DEBUG_UART=y',
                     'CONFIG_BOARD_EARLY_INIT_F=y',
                     '# CONFIG_DISPLAY_BOARDINFO is not set',
                     'CONFIG_DISPLAY_BOARDINFO_LATE=y']:
        assert required in config, (name, required)
print('EARLY_DM_HOOK_PASS missing-node, bind-error, DMC-only, full/minimal-config')

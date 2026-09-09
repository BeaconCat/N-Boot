# SPDX-License-Identifier: GPL-2.0+
"""Exercise the bootloader's actual persistent-request consumer."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'cmd/bootnuttx.c').read_text()
match = re.search(r'int nboot_bootctrl_take_request\(void\)\n\{', source)
assert match
end = match.end()
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[match.start():end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define ARCH_DMA_MINALIGN 64
#define NBOOT_REBOOT_MAGIC 0x4e425200u
#define NBOOT_REBOOT_MAGIC_MASK 0xffffff00u
#define NBOOT_REBOOT_CONSOLE 1
#define NBOOT_REBOOT_SLOT_B 4
#define le32_to_cpu(v) (v)
struct k7_bootctrl_disk { unsigned char padding[4]; };
struct disk_partition { int dummy; };
struct nboot_storage { void *desc; };
static u32 saved;
static int writes, error;
static void *memalign(size_t alignment, size_t size)
{ (void)alignment; return calloc(1, size); }
static int nboot_storage_open_boot(struct nboot_storage *s)
{ s->desc = NULL; return 0; }
static int part_get_info_by_name(void *d, const char *n, struct disk_partition *p)
{ (void)d; (void)p; assert(!strcmp(n,"bootctrl")); return 0; }
static int k7_bootctrl_read(void *d, struct disk_partition *p,
                           struct k7_bootctrl_disk *r, int *selected)
{ (void)d; (void)p; *selected=1; memcpy(r[1].padding,&saved,4); return 0; }
static int k7_bootctrl_write(void *d, struct disk_partition *p,
                            struct k7_bootctrl_disk *r, int selected)
{ (void)d; (void)p; writes++; assert(selected==1);
  assert(!memcmp(r[1].padding,"\0\0\0\0",4));
  if (error) return -1;
  memcpy(&saved,r[1].padding,4); return 0; }
''' + function + r'''
int main(void)
{
  for (unsigned int target=1; target<=4; target++) {
    saved=NBOOT_REBOOT_MAGIC|target; writes=0; error=0;
    assert(nboot_bootctrl_take_request()==(int)target);
    assert(saved==0 && writes==1);
    assert(nboot_bootctrl_take_request()==0 && writes==1);
  }
  saved=0x12345678; writes=0;
  assert(nboot_bootctrl_take_request()==0 && writes==0);
  saved=NBOOT_REBOOT_MAGIC|99; writes=0;
  assert(nboot_bootctrl_take_request()==0 && writes==1 && saved==0);
  saved=NBOOT_REBOOT_MAGIC|1; writes=0; error=1;
  assert(nboot_bootctrl_take_request()==0 && writes==1 && saved!=0);
  return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(program)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('REBOOT_REQUEST_PASS targets=4 one-shot invalid-target write-failure')

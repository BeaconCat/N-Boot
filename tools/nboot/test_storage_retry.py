#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Run the real boot-medium selection and retry policy against fake cards."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                    pathlib.Path(__file__).resolve().parents[2]).resolve()
common = r'''
#ifndef TEST_COMMON_H
#define TEST_COMMON_H
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long ulong;
typedef unsigned long lbaint_t;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define ALLOC_CACHE_ALIGN_BUFFER(type,name,size) type name[size]
struct blk_desc { ulong blksz; int devnum; };
struct disk_partition { lbaint_t start, size; };
#define IS_ENABLED(option) 1
enum bus_mode { MMC_HS_52 = 3, MMC_HS_200 = 10, MMC_MODES_END = 13 };
struct mmc { unsigned int has_init; int devnum; struct blk_desc desc;
             enum bus_mode user_speed_mode; };
int puts(const char *text);
struct mmc *find_mmc_device(int devnum);
int mmc_init(struct mmc *mmc);
struct blk_desc *mmc_get_blk_desc(struct mmc *mmc);
int part_get_info_by_name(struct blk_desc *desc, const char *name,
                          struct disk_partition *partition);
ulong blk_dread(struct blk_desc *desc, lbaint_t start, lbaint_t blocks,
                void *buffer);
int env_set(const char *name, const char *value);
void mdelay(unsigned long ms);
#endif
'''
test = r'''
#include "common.h"
#include "storage.c"

enum { ABSENT, SILENT, UNREADABLE, FOREIGN, OURS };
static struct mmc cards[2] = { { .devnum = 0, .desc = { 512, 0 } },
                               { .devnum = 1, .desc = { 512, 1 } } };
static int state[2], heal_after[2], inits[2];
static unsigned long waited, delays[8];
static int delay_count;

struct mmc *find_mmc_device(int devnum) { return &cards[devnum]; }
struct blk_desc *mmc_get_blk_desc(struct mmc *mmc) { return &mmc->desc; }
int env_set(const char *name, const char *value)
{ (void)name; (void)value; return 0; }
void mdelay(unsigned long ms) { waited += ms; delays[delay_count++] = ms; }

/* A card heals after it has been initialised from scratch often enough. */
int mmc_init(struct mmc *mmc)
{
  int dev = mmc->devnum;

  if (mmc->has_init)
    return 0;
  inits[dev]++;
  if (heal_after[dev] && inits[dev] > heal_after[dev])
    state[dev] = OURS;
  if (state[dev] == ABSENT)
    return -ENOMEDIUM;
  if (state[dev] == SILENT)
    return -ETIMEDOUT;
  mmc->has_init = 1;
  return 0;
}

int part_get_info_by_name(struct blk_desc *desc, const char *name,
                          struct disk_partition *partition)
{
  static const struct { const char *name; lbaint_t start, size; } layout[] = {
    { "uboot", 0x4000, 8192 }, { "trust", 0x6000, 8192 },
    { "bootctrl", 0x8000, 2048 }, { "nuttx_a", 0x9000, 131072 },
    { "nuttx_b", 0x29000, 131072 },
  };
  unsigned int i;

  if (state[desc->devnum] != OURS)
    return -ENOENT;
  for (i = 0; i < ARRAY_SIZE(layout); i++)
    if (!strcmp(name, layout[i].name)) {
      partition->start = layout[i].start;
      partition->size = layout[i].size;
      return i + 1;
    }
  return -ENOENT;
}

ulong blk_dread(struct blk_desc *desc, lbaint_t start, lbaint_t blocks,
                void *buffer)
{
  assert(!start && blocks == 1);
  memset(buffer, 0, desc->blksz);
  return state[desc->devnum] == UNREADABLE ? 0 : 1;
}

static void world(int sd, int emmc)
{
  memset(inits, 0, sizeof(inits));
  memset(heal_after, 0, sizeof(heal_after));
  cards[0].has_init = cards[1].has_init = 0;
  cards[0].user_speed_mode = cards[1].user_speed_mode = MMC_MODES_END;
  state[0] = sd; state[1] = emmc;
  selected_devnum = target_devnum = -1;
  retries_left = NBOOT_STORAGE_RETRIES;
  waited = 0; delay_count = 0;
}

int main(void)
{
  struct nboot_storage storage;
  int ret, rounds;

  /* The ordinary cold boot: no SD card, eMMC answers.  No wait, one
   * initialisation, and the retry budget is untouched.
   */
  world(ABSENT, OURS);
  assert(!nboot_storage_open_boot(&storage) && storage.devnum == 1);
  assert(storage.medium == NBOOT_MEDIUM_EMMC && !waited && inits[1] == 1);
  assert(retries_left == NBOOT_STORAGE_RETRIES);
  assert(!nboot_storage_retry("test", 0) && !waited);

  /* A recovery SD card still wins, and an empty SD slot alone is never a
   * reason to wait.
   */
  world(OURS, OURS);
  assert(!nboot_storage_open_boot(&storage) && storage.devnum == 0 && !waited);
  world(ABSENT, FOREIGN);
  ret = nboot_storage_open_boot(&storage);
  assert(ret == -ENOENT && !nboot_storage_retry("test", ret) && !waited);

  /* Warm reset: the eMMC does not initialise at first.  The loader is told
   * to go round again, each round forgets the card, and the third full
   * initialisation succeeds.
   */
  world(ABSENT, SILENT);
  heal_after[1] = 2;
  for (rounds = 0; (ret = nboot_storage_open_boot(&storage)); rounds++) {
    assert(ret == -EIO);
    assert(nboot_storage_retry("test", ret));
    assert(!cards[1].has_init);
  }
  assert(rounds == 2 && storage.devnum == 1 && inits[1] == 3);
  assert(delays[0] == 200 && delays[1] == 400 && waited == 600);
  /* Two retries are still at full speed. */
  assert(cards[1].user_speed_mode == MMC_MODES_END);

  /* Initialised, selected, and then it stops reading (a failed transfer
   * leaves the card in the data state).  Only the selected medium is
   * re-initialised, and it comes back.
   */
  world(ABSENT, OURS);
  assert(!nboot_storage_open_boot(&storage) && cards[1].has_init);
  state[1] = UNREADABLE;
  heal_after[1] = 1;
  ret = nboot_storage_open_boot(&storage);
  assert(ret == -EIO && nboot_storage_retry("test", ret));
  assert(!cards[1].has_init && inits[0] == 1);
  assert(!nboot_storage_open_boot(&storage) && inits[1] == 2);

  /* A medium that stays silent costs the budget once: five waits, 6.2 s,
   * and whoever asks next gets a plain failure without another wait.
   */
  world(ABSENT, SILENT);
  for (rounds = 0; (ret = nboot_storage_open_boot(&storage)) == -EIO &&
                   nboot_storage_retry("test", ret); rounds++)
    ;
  assert(rounds == NBOOT_STORAGE_RETRIES && ret == -EIO && waited == 6200);
  assert(delays[4] == 3200);
  /* From the third retry the eMMC, and only the eMMC, gives up HS200. */
  assert(cards[1].user_speed_mode == MMC_HS_52);
  assert(cards[0].user_speed_mode == MMC_MODES_END);
  assert(!nboot_storage_retry("again", -EIO) && waited == 6200);

  puts("STORAGE_RETRY_PASS cold-boot sd-first foreign warm-reset data-state budget speed-step");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nboot-storage-") as tmp:
    root = pathlib.Path(tmp)
    (root / 'common.h').write_text(common)
    for name in ('fb_mmc.h', 'env.h', 'memalign.h', 'part.h', 'blk.h', 'mmc.h',
                 'linux/delay.h', 'linux/kernel.h', 'linux/types.h'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "common.h"\n')
    (root / 'linux/errno.h').write_text('#include_next <linux/errno.h>\n')
    for name in ('nboot_storage.h', 'nboot_contract.h'):
        (root / name).write_text(
            f'#include "common.h"\n#include "{repo}/include/{name}"\n')
    shutil.copyfile(repo / 'arch/arm/mach-rockchip/rk3576/nboot_storage.c',
                    root / 'storage.c')
    (root / 'test.c').write_text(test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-I', str(root),
                    str(root / 'test.c'), '-o', str(root / 'test')],
                   check=True)
    subprocess.run([str(root / 'test')], check=True)

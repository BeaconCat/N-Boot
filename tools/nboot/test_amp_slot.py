#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Exercise the real active-slot loader with mocked block I/O."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "cmd/bootnuttx.c").read_text()
types = source[source.index("struct k7_slot_disk {"):source.index("_Static_assert")]
defines = "\n".join(re.findall(r"^#define K7_.*$", source, re.M))


def extract(pattern):
    match = re.search(pattern, source)
    assert match
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


# One attempt at the slot, and the loop that repeats it while the medium does
# not answer.
loader = (extract(r"static int nboot_amp_load_slot_once\([^;]*?\)\n\{") + "\n" +
          extract(r"int nboot_amp_load_slot\([^;]*?\)\n\{"))
program = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned long ulong;
typedef unsigned long lbaint_t;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define __packed __attribute__((packed))
#define ARCH_DMA_MINALIGN 64
#define NBOOT_MEDIUM_EMMC 2
#define le64_to_cpu(v) ((u64)(v))
#define DIV_ROUND_UP(v,a) (((v)+(a)-1)/(a))
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
''' + defines + "\n" + types + r'''
struct blk_desc { ulong blksz; };
struct disk_partition { ulong start, size; };
struct nboot_storage { struct blk_desc *desc; int medium; };
struct nboot_amp_origin { unsigned int medium, slot; u64 generation; };
static struct nboot_amp_origin origin;
static struct blk_desc disk = {512};
static struct k7_bootctrl_disk saved;
static unsigned char payload[1024], staging[1024];
static int writes, reads, fault, heals, budget, retries;
/* The real one also waits and re-initialises the card; what matters to the
 * loader is only when it is told to go round again.
 */
static bool nboot_storage_retry(const char *who,int error)
{ assert(!strcmp(who,"bootamp") && error);
  if(error!=-EIO || !budget)return false;
  budget--;retries++;return true; }
static int nboot_storage_open_boot(struct nboot_storage *s)
{ s->desc=&disk;s->medium=2;return fault==1?-ENODEV:0; }
static int part_get_info_by_name(struct blk_desc *d,const char *name,
                                struct disk_partition *p)
{ assert(d==&disk);p->size=2048;p->start=100;
  if(strcmp(name,"bootctrl")) {
    assert(!strcmp(name,"amp_b"));p->start=400;
    if(fault==2)return -ENOENT;
  }
  return 1;
}
static int k7_bootctrl_read(struct blk_desc *d,struct disk_partition *p,
                            struct k7_bootctrl_disk *r,int *selected)
{ (void)d;assert(p->start==100);*selected=1;r[1]=saved;return 0; }
static int k7_bootctrl_write(struct blk_desc *d,struct disk_partition *p,
                             struct k7_bootctrl_disk *r,int selected)
{ (void)d;assert(p->start==100 && selected==1);writes++;
  if(fault==3)return -EIO;
  r[selected].generation++;saved=r[selected];return 0; }
static void *map_sysmem(ulong address,ulong size)
{ assert(address==0x60000000 && size==1024);return staging; }
static void unmap_sysmem(void *p) {assert(p==staging);}
static ulong blk_dread(struct blk_desc *d,ulong start,ulong blocks,void *p)
{ (void)d;assert(start==400 && blocks==2);reads++;
  if(fault==4){if(heals)fault=0;return 0;}
  memcpy(p,payload,1024);return blocks; }
static int hash_block(const char *name,const void *p,unsigned int size,
                       void *digest,int *digest_size)
{ assert(!strcmp(name,"sha256") && size==513 && *digest_size==32);
  SHA256(p,size,digest);return 0; }
static void flush_cache(ulong address,ulong size)
{ assert(address==0x60000000 && size==576); }
''' + loader + r'''
static void setup(int confirmed,int tries)
{
  memset(&saved,0,sizeof(saved));memset(payload,0x5a,sizeof(payload));
  memset(&saved.domains[0],0x33,sizeof(saved.domains[0]));
  saved.domains[1].active_slot=1;saved.generation=41;
  memset(&origin,0xff,sizeof(origin));
  struct k7_slot_disk *s=&saved.domains[1].slots[1];
  s->priority=15;s->successful=confirmed;s->tries_remaining=tries;
  s->image_size=513;s->image_version=7;
  SHA256(payload,513,s->sha256);writes=reads=fault=heals=budget=retries=0;
}
int main(void)
{
  ulong size;
  setup(0,1);
  struct k7_domain_disk nuttx=saved.domains[0];
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0 && size==513);
  assert(writes==1 && reads==1 && !saved.domains[1].slots[1].tries_remaining);
  assert(!memcmp(&nuttx,&saved.domains[0],sizeof(nuttx)));
  /* What the handoff will say: the medium, the slot that was loaded, and the
   * generation as the trial write left it, not as it was read.
   */
  assert(origin.medium==2 && origin.slot==1 && origin.generation==42);
  /* Consuming the last trial does not make the slot ineligible: eligibility
   * is priority alone, so an activated slot keeps booting whether or not an
   * OS ever confirmed it.  A freshly activated slot has zero tries by
   * construction, and requiring one would leave the AMP domain permanently
   * on the other side.  The trial is still consumed exactly once.
   */
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0 && size==513);
  assert(writes==1 && reads==2 && !saved.domains[1].slots[1].tries_remaining);
  setup(1,0);
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0);
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0);
  assert(writes==0 && reads==2);
  setup(0,1);fault=3;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EIO && !reads);
  setup(0,1);fault=4;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EIO);
  /* A slot that did not load reports no origin. */
  assert(origin.medium==0xffffffffu && origin.slot==0xffffffffu);
  assert(writes==1 && !saved.domains[1].slots[1].tries_remaining);
  setup(0,1);payload[0]^=1;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EKEYREJECTED);
  assert(writes==1 && !saved.domains[1].slots[1].tries_remaining);
  setup(0,1);
  assert(nboot_amp_load_slot(0x60000000,512,&size,&origin)==-EFBIG && !writes && !reads);
  setup(1,0);saved.domains[1].slots[1].image_size=0;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-ENOENT && !writes);
  /* An unconfirmed slot with no tries left still loads.  That combination is
   * exactly what activation produces, so refusing it would make every
   * freshly activated AMP slot unbootable.  Note the write count: with the
   * trial already used up there is nothing to decrement, so bootctrl must
   * not be rewritten on this path.
   */
  setup(0,0);
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0 && size==513);
  assert(writes==0 && reads==1);
  setup(0,1);saved.domains[1].active_slot=2;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EINVAL && !writes);
  setup(0,1);fault=1;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-ENODEV && !writes);
  setup(0,1);fault=2;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-ENOENT && !writes);
  /* A read that fails once: the slot loads on the second attempt, and the
   * boot still costs exactly one trial.
   */
  setup(0,1);fault=4;heals=1;budget=5;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==0 && size==513);
  assert(retries==1 && reads==2 && writes==1);
  assert(!saved.domains[1].slots[1].tries_remaining && origin.slot==1);
  /* A medium that never answers uses up what it is given and no more. */
  setup(0,1);fault=4;budget=2;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EIO);
  assert(retries==2 && reads==3 && writes==1);
  /* An answer is not retried: no such device, empty slot, bad digest,
   * wrong size.
   */
  setup(0,1);fault=1;budget=5;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-ENODEV && !retries);
  setup(1,0);saved.domains[1].slots[1].image_size=0;budget=5;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-ENOENT && !retries);
  setup(0,1);payload[0]^=1;budget=5;
  assert(nboot_amp_load_slot(0x60000000,1024,&size,&origin)==-EKEYREJECTED);
  assert(!retries && reads==1);
  setup(0,1);budget=5;
  assert(nboot_amp_load_slot(0x60000000,512,&size,&origin)==-EFBIG && !retries);
  puts("AMP_SLOT_PASS trial confirmed empty bounds read hash write failures retry");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="amp-slot-test-") as directory:
    path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    path.write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path),
                    "-lcrypto", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

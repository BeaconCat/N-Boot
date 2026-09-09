#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Run the real bootamp control flow with fake firmware/MMIO, never hardware."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1]).resolve()
common = r'''
#ifndef TEST_COMMON_H
#define TEST_COMMON_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
typedef unsigned long ulong;
struct udevice;
#define UCLASS_FIRMWARE 1
int uclass_get_device_by_name(int, const char *, struct udevice **);
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
struct vring_desc {u64 addr;u32 len;u16 flags,next;};
struct vring_avail {u16 flags,idx,ring[];};
#define VRING_DESC_F_WRITE 2
typedef ulong phys_addr_t;
typedef ulong phys_size_t;
#define LMB_MEM_ALLOC_ADDR 1
#define LMB_NONE 0
#define LMB_NOMAP 2
int lmb_alloc_mem(int,ulong,phys_addr_t *,phys_size_t,u32);
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define min(x,y) ((x)<(y)?(x):(y))
#define CONFIG_NR_DRAM_BANKS 1
#define CONFIG_STACK_SIZE 0x10000
#define GD_FLG_SKIP_RELOC 1
#define CMD_RET_USAGE -1
#define CMD_RET_FAILURE 1
#define CMD_RET_SUCCESS 0
#define U_BOOT_CMD(...)
#define DECLARE_GLOBAL_DATA_PTR
#define BOOTM_STATE_START 1
#define BOOTM_STATE_PRE_LOAD 2
#define BOOTM_STATE_FINDOS 4
#define BOOTM_STATE_FINDOTHER 8
#define BOOTM_STATE_MEASURE 16
#define BOOTM_STATE_LOADOS 32
#define BOOTM_STATE_RAMDISK 64
#define BOOTM_STATE_FDT 128
#define BOOTM_STATE_OS_PREP 256
#define PSCI_0_2_FN64_AFFINITY_INFO 0xc4000004UL
#define PSCI_0_2_FN64_CPU_ON 0xc4000003UL
#define PSCI_0_2_AFFINITY_LEVEL_OFF 1
#define PSCI_RET_SUCCESS 0
struct cmd_tbl {int unused;};
struct bootm_info {const char *cmd_name; const char *addr_img;};
struct arm_smccc_res {ulong a0;};
struct test_gd {ulong start_addr_sp,initial_relocaddr,flags; struct {ulong start,size;} dram[1];};
extern struct test_gd *gd;
extern struct test_images {ulong ep;char *ft_addr;} images;
int strict_strtoul(const char *,unsigned int,ulong *);
ulong read_mpidr(void);
ulong invoke_psci_fn(ulong,ulong,ulong,ulong);
void bootm_init(struct bootm_info *);
int bootm_run_states(struct bootm_info *,int);
void arm_smccc_smc(ulong,ulong,ulong,ulong,ulong,ulong,ulong,ulong,struct arm_smccc_res *);
void reset_cpu(void);
void hang(void);
char *env_get(const char *);
int env_set(const char *,const char *);
int fdt_num_mem_rsv(const void *);
int fdt_get_mem_rsv(const void *,int,uint64_t *,uint64_t *);
int fdt_add_mem_rsv(void *,uint64_t,uint64_t);
int fdt_path_offset(const void *,const char *);
int fdt_setprop_string(void *,int,const char *,const char *);
int fdt_delprop(void *,int,const char *);
int fdt_totalsize(const void *);
void fdt_set_boot_cpuid_phys(void *,u32);
ulong map_to_sysmem(const void *);
u32 readl(ulong);
u8 readb(ulong);
u16 readw(ulong);
u64 readq(ulong);
void writel(u32,ulong);
void dsb(void);
ulong get_timer(ulong);
void udelay(ulong);
ulong bootm_disable_interrupts(void);
void bootm_final(int);
void cleanup_before_linux(void);
void test_entry(void);
int nboot_amp_validate(const void *,u32);
int nboot_amp_validate_fdt(const void *,u32);
#endif
'''
test = r'''
#include <setjmp.h>
#include "common.h"
#include "command.c"
struct test_gd data, *gd=&data;
struct test_images images;
static jmp_buf finish;
static int mode, states, sip, launched, quiesced, cleaned, irqoff, reset, entered, writes, mailbox, reserved;
static ulong tick, reserve_start, reserve_size;
static u32 control, mark;
static char dummy_fdt[8192];
enum {GOOD, BAD_FIT, BUSY_CPU, RESERVE_FAIL, LOST_RESERVE, SIP_FAIL,
      ARM_FAIL, CPU_ON_FAIL, TIMEOUT, BAD_ROUTE, BAD_PRIORITY, PREP_FAIL,
      BAD_FDT, BAD_BOOTARGS, BAD_CPU, ENV_RELOC, RETURN_ENTRY, MAILBOX_STALE, LMB_FAIL,
      TRANSPORT_TIMEOUT, BAD_LINK, BAD_MAGIC, BAD_BUFFERS, BAD_AVAIL, BAD_FLAGS, BAD_LENGTH};
int strict_strtoul(const char *s,unsigned int base,ulong *out) {char *e;*out=strtoul(s,&e,base);return *e?-1:0;}
ulong read_mpidr(void) {return mode==BAD_CPU?0x100:0;}
static bool psci_probed;
int uclass_get_device_by_name(int id,const char *name,struct udevice **dev) {
 assert(id==UCLASS_FIRMWARE && !strcmp(name,"psci"));
 *dev=NULL;psci_probed=true;return 0;
}
ulong invoke_psci_fn(ulong op,ulong cpu,ulong entry,ulong arg) {
 assert(psci_probed);
 if(op==PSCI_0_2_FN64_AFFINITY_INFO)return mode==BUSY_CPU?0:1;
 assert(op==PSCI_0_2_FN64_CPU_ON && cpu==0x100 && entry==0x42000000 && arg==0);
 assert(cleaned && quiesced && irqoff && mark==0 && control==0 && sip==3 && reserve_size && mailbox==3);
 launched++;return mode==CPU_ON_FAIL?(ulong)-1:0;
}
void bootm_init(struct bootm_info *b) {memset(b,0,sizeof(*b));}
int bootm_run_states(struct bootm_info *b,int s) {
 assert(!launched && !writes && !sip);
 assert(!strcmp(b->cmd_name,"bootamp"));states++;
 if(s&BOOTM_STATE_FINDOTHER)assert(states==1 && b->addr_img && strstr(b->addr_img,"#conf"));
 if(s&BOOTM_STATE_RAMDISK)assert(reserved==6);
 if(s&BOOTM_STATE_OS_PREP){assert(reserve_size);if(mode==PREP_FAIL)return 1;}
 images.ep=0x42000000;images.ft_addr=dummy_fdt;return 0;
}
int lmb_alloc_mem(int type,ulong align,phys_addr_t *base,phys_size_t size,u32 flags) {
 assert(type==LMB_MEM_ALLOC_ADDR && !align && !launched);
 if(mode==LMB_FAIL)return -12;
 if(reserved==0)assert(states==0 && *base==0x60000000 && size==0x1000 && flags==0);
 else assert(states==1);
 if(reserved==1)assert(*base==NBOOT_AMP_LINUX_ENTRY && size==0x5000000 && flags==0);
 if(reserved==2)assert(*base==0x47800000 && size==0x800000 && flags==2);
 if(reserved==3)assert(*base==NBOOT_AMP_NUTTX_ENTRY && size==0x1000000 && flags==2);
 if(reserved==4)assert(*base==0x4f000000 && size==0x1000000 && flags==0);
 if(reserved==5)assert(*base==0x50000000 && size==0x10000000 && flags==0);
 reserved++;return 0;
}
void arm_smccc_smc(ulong id,ulong op,ulong cpu,ulong a,ulong b,ulong c,ulong d,ulong e,struct arm_smccc_res *r) {
 assert(id==0x82000022 && cpu==0x100 && !c && !d && !e && !launched && reserve_size);
 assert(op==(ulong)sip);if(op==0)assert(a==0x16 && b==0);
 if(op==1)assert(a==0x7f000000 && b==0);if(op==2)assert(a==0 && b==0);
 sip++;r->a0=mode==SIP_FAIL?(ulong)-1:0;
}
void reset_cpu(void) {reset++;longjmp(finish,2);}
void hang(void) {assert(0);}
char *env_get(const char *s) {(void)s;return mode==ENV_RELOC?"ffffffffffffffff":NULL;}
int env_set(const char *s,const char *v) {assert(!strcmp(s,"bootargs"));if(v)assert(strstr(v,"clk_ignore_unused")&&!strstr(v,"earlycon"));return mode==BAD_BOOTARGS?-1:0;}
int fdt_num_mem_rsv(const void *f) {(void)f;return mode==LOST_RESERVE?0:1;}
int fdt_get_mem_rsv(const void *f,int i,uint64_t *a,uint64_t *s) {(void)f;assert(i==0);*a=reserve_start;*s=reserve_size;return 0;}
int fdt_add_mem_rsv(void *f,uint64_t a,uint64_t s) {(void)f;assert(states==2);reserve_start=a;reserve_size=s;return mode==RESERVE_FAIL?-1:0;}
int fdt_path_offset(const void *f,const char *p) {(void)f;assert(!strcmp(p,"/chosen"));return 1;}
int fdt_setprop_string(void *f,int n,const char *p,const char *v) {(void)f;(void)n;assert(!strcmp(p,"bootargs")&&strstr(v,"clk_ignore_unused")&&!strstr(v,"earlycon"));return mode==BAD_BOOTARGS?-1:0;}
int fdt_delprop(void *f,int n,const char *p) {(void)f;(void)n;(void)p;return 0;}
int fdt_totalsize(const void *f) {(void)f;return 8192;}
void fdt_set_boot_cpuid_phys(void *f,u32 c) {(void)f;assert(c==0x100);}
ulong map_to_sysmem(const void *p) {(void)p;return 0x7f000000;}
u32 readl(ulong a) {
 if(a==NBOOT_AMP_MBOX3+4)return mode==MAILBOX_STALE?1:(launched && mode!=TRANSPORT_TIMEOUT);
 if(a==NBOOT_AMP_MBOX0+0x14 || a==NBOOT_AMP_MBOX3+0x14)return 0;
 if(a==NBOOT_AMP_MBOX3+8)return mode==BAD_LINK?4:3;
 if(a==NBOOT_AMP_MBOX3+12)return mode==BAD_MAGIC?0:0x524d5347;
 if(a>=NBOOT_AMP_VRING0_BASE && a<NBOOT_AMP_VRING0_BASE+64*16) {
  assert((a-NBOOT_AMP_VRING0_BASE)%16==8);return mode==BAD_LENGTH?1024:512;
 }
 if(a==NBOOT_AMP_GICD+4)return 15;
 if(a==NBOOT_AMP_GICD)return launched && mode!=TIMEOUT?1:control;
 assert(a==NBOOT_AMP_GICD+0x5fc);
 if(mode==ARM_FAIL)return 0xa0a0a0a0;
 return launched && mode!=TIMEOUT?0xa0a0a0a0:mark;
}
u64 readq(ulong a) {assert(a>=NBOOT_AMP_VRING0_BASE && a<NBOOT_AMP_VRING0_BASE+64*16 && !((a-NBOOT_AMP_VRING0_BASE)%16));return NBOOT_AMP_BUFFER_BASE+(a-NBOOT_AMP_VRING0_BASE)/16*512+(mode==BAD_BUFFERS?NBOOT_AMP_BUFFER_LIMIT:0);}
u16 readw(ulong a) {
 if(a==NBOOT_AMP_VRING0_BASE+64*16+2)return mode==BAD_AVAIL?0:64;
 assert(a>=NBOOT_AMP_VRING0_BASE && a<NBOOT_AMP_VRING0_BASE+64*16 && (a-NBOOT_AMP_VRING0_BASE)%16==12);
 return mode==BAD_FLAGS?0:VRING_DESC_F_WRITE;
}
u8 readb(ulong a) {
 if(a==NBOOT_AMP_GICD+0x800)return 1;
 if(a==NBOOT_AMP_GICD+0x800+108 || a==NBOOT_AMP_GICD+0x800+174)
  return mode==BAD_ROUTE?8:1;
 assert(a==NBOOT_AMP_GICD+0x400+108 || a==NBOOT_AMP_GICD+0x400+174);
 return mode==BAD_PRIORITY?0xa0:0x80;
}
void writel(u32 v,ulong a) {
 assert(cleaned && quiesced && irqoff && !launched);writes++;
 if(a==NBOOT_AMP_MBOX3+NBOOT_AMP_MBOX_A2B_INTEN){assert(v==0x10001);return;}
 if(a==NBOOT_AMP_MBOX_GATE){assert(v==NBOOT_AMP_MBOX_GATE_ON);return;}
 if(a==NBOOT_AMP_MBOX3+4 || a==NBOOT_AMP_MBOX0+0x14 || a==NBOOT_AMP_MBOX3+0x14){assert(v==1);mailbox++;return;}
 if(a==NBOOT_AMP_GICD)control=v;else {assert(a==NBOOT_AMP_GICD+0x5fc);mark=v;}
}
void dsb(void) {}
ulong get_timer(ulong b) {tick+=1000;return tick-b;}
void udelay(ulong t) {(void)t;}
ulong bootm_disable_interrupts(void) {irqoff=1;return 0;}
void bootm_final(int f) {assert(!f && irqoff && sip==3);quiesced=1;}
void cleanup_before_linux(void) {assert(quiesced && irqoff && !launched);cleaned=1;}
int nboot_amp_validate(const void *f,u32 s) {(void)f;assert(s==4096 && !states && !writes);return mode==BAD_FIT?-22:0;}
int nboot_amp_validate_fdt(const void *f,u32 s) {(void)f;(void)s;return mode==BAD_FDT?-22:0;}
void test_entry(void) {assert(launched==1 && cleaned && reserve_size && readl(NBOOT_AMP_MBOX3+4)==1);entered=1;if(mode!=RETURN_ENTRY)longjmp(finish,1);}
int main(void) {
 int m,result,ret;
 char *args[]={"bootamp","60000000","1000","check"};
 for(m=GOOD;m<=BAD_LENGTH;m++) {
  mode=m;states=sip=launched=quiesced=cleaned=irqoff=reset=entered=writes=mailbox=reserved=0;tick=0;reserve_size=0;psci_probed=false;
  control=1;mark=0xa0a0a0a0;
  data=(struct test_gd){.start_addr_sp=0xbf000000,.initial_relocaddr=0xc0000000,.dram={{0x40000000,0x80000000}}};
  result=setjmp(finish);
  if(!result) {
   ret=do_bootamp(NULL,0,m==GOOD?4:3,args);
   if(m==GOOD){assert(!ret && !states && !writes && !sip);ret=do_bootamp(NULL,0,3,args);}
   assert(ret==CMD_RET_FAILURE && !reset && !launched && !writes);
  } else if(result==1) {assert(m==GOOD && entered && !reset);}
  else {assert(reset==1 && (m==SIP_FAIL||m==ARM_FAIL||m==CPU_ON_FAIL||m==TIMEOUT||m==BAD_ROUTE||m==BAD_PRIORITY||m==RETURN_ENTRY||m==MAILBOX_STALE||m>=TRANSPORT_TIMEOUT));}
  printf("BOOT_FLOW mode=%d PASS\n",m);
 }
 puts("NBOOT_AMP_FLOW_PASS cases=26");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nboot-flow-") as tmp:
    root=pathlib.Path(tmp)
    (root/'common.h').write_text(common)
    for name in ('dm.h','asm/global_data.h','asm/io.h','asm/system.h','bootm.h','command.h','cpu_func.h','env.h','hang.h','image.h','linux/arm-smccc.h','linux/delay.h','linux/errno.h','linux/libfdt.h','linux/psci.h','mapmem.h','lmb.h','nboot_amp.h','time.h','virtio_ring.h'):
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('#include_next <linux/errno.h>\n' if name=='linux/errno.h' else '#include "common.h"\n')
    (root/'nboot_amp.h').write_text(f'#include "common.h"\n#include "{repo}/include/nboot_amp.h"\n#undef NBOOT_AMP_NUTTX_ENTRY\n#define NBOOT_AMP_NUTTX_ENTRY ((ulong)test_entry)\n')
    shutil.copyfile(repo/'cmd/bootamp.c',root/'command.c')
    (root/'test.c').write_text(test)
    subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-misleading-indentation','-I',str(root),str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)

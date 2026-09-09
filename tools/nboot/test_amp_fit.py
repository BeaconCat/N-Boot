#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Exercise the real AMP validator with libfdt and SHA256 on generated FITs."""
import ctypes
import gzip
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

repo = pathlib.Path(sys.argv[1]).resolve()
dtc = repo / "scripts/dtc/libfdt"
with tempfile.TemporaryDirectory(prefix="nboot-amp-test-") as directory:
    root = pathlib.Path(directory)
    for name in ("linux", "asm"):
        (root / name).mkdir()
    (root / "linux/types.h").write_text("#include <stdint.h>\n#include <stdbool.h>\ntypedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef unsigned long ulong;\n")
    (root / "linux/errno.h").write_text("#include_next <linux/errno.h>\n")
    (root / "linux/string.h").write_text("#include <string.h>\n")
    (root / "linux/libfdt.h").write_text("#include <libfdt.h>\n")
    (root / "asm/global_data.h").write_text("#define DECLARE_GLOBAL_DATA_PTR\n#define gd_fdt_blob() ((void *)0)\n")
    (root / "asm/unaligned.h").write_text("#include <stdint.h>\n#include <string.h>\n#include <endian.h>\nstatic inline uint64_t get_unaligned_le64(const void *p) {uint64_t v;memcpy(&v,p,8);return le64toh(v);}\n")
    shutil.copyfile(repo / "include/nboot_amp.h", root / "nboot_amp.h")
    shutil.copyfile(repo / "lib/nboot_amp.c", root / "validator.c")
    (root / "image.h").write_text('''#include <linux/types.h>
#include <stddef.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define IH_TYPE_KERNEL 2
#define IH_TYPE_RAMDISK 3
#define IH_TYPE_FIRMWARE 5
#define IH_TYPE_FLATDT 8
#define IH_COMP_NONE 0
#define IH_COMP_GZIP 1
#define IH_ARCH_ARM64 22
#define FIT_HASH_NODENAME "hash"
int fit_image_get_data_position(const void *, int, int *);
int fit_image_get_data_size(const void *, int, int *);
int fit_image_get_load(const void *, int, ulong *);
int fit_image_get_entry(const void *, int, ulong *);
int fit_image_check_type(const void *, int, u8);
int fit_image_check_arch(const void *, int, u8);
int fit_image_check_comp(const void *, int, u8);
int fit_image_verify_with_data(const void *, int, const void *, const void *, size_t);
''')
    (root / "helpers.c").write_text('''#include <image.h>
#include <libfdt.h>
#include <string.h>
#include <openssl/sha.h>
int test_setprop(void *f,int n,const char *p,const void *v,int len) {return fdt_setprop(f,n,p,v,len);}
static int integer(const void *f, int n, const char *p, ulong *v) {
 int l; const fdt32_t *a=fdt_getprop(f,n,p,&l);
 if (!a || (l!=4 && l!=8)) return -1;
 *v=fdt32_to_cpu(a[0]); if(l==8)*v=(*v<<32)|fdt32_to_cpu(a[1]); return 0;
}
int fit_image_get_data_position(const void *f,int n,int *v) {ulong x;int r=integer(f,n,"data-position",&x);if(!r)*v=x;return r;}
int fit_image_get_data_size(const void *f,int n,int *v) {ulong x;int r=integer(f,n,"data-size",&x);if(!r)*v=x;return r;}
int fit_image_get_load(const void *f,int n,ulong *v) {return integer(f,n,"load",v);}
int fit_image_get_entry(const void *f,int n,ulong *v) {return integer(f,n,"entry",v);}
static int eq(const void *f,int n,const char *p,const char *s) {int l;const char *v=fdt_getprop(f,n,p,&l);return v&&l==(int)strlen(s)+1&&!memcmp(v,s,l);}
int fit_image_check_type(const void *f,int n,u8 t) {const char *s=t==2?"kernel":t==3?"ramdisk":t==5?"firmware":"flat_dt";return eq(f,n,"type",s);}
int fit_image_check_arch(const void *f,int n,u8 t) {(void)t;return eq(f,n,"arch","arm64");}
int fit_image_check_comp(const void *f,int n,u8 c) {return eq(f,n,"compression",c?"gzip":"none");}
int fit_image_verify_with_data(const void *f,int n,const void *control,const void *data,size_t size) {
 int h,l,found=0; unsigned char digest[32]; (void)control; SHA256(data,size,digest);
 fdt_for_each_subnode(h,f,n) {const char *name=fdt_get_name(f,h,0);const void *v;
 if(strncmp(name,"hash",4))continue; if(!eq(f,h,"algo","sha256"))return 0;
 v=fdt_getprop(f,h,"value",&l); if(!v||l!=32||memcmp(v,digest,32))return 0;found=1;}
 return found;
}
''')
    sources = [str(p) for p in dtc.glob("*.c")]
    libpath = root / "validator.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-g", "-I", str(root),
                    "-I", str(dtc), str(root / "validator.c"), str(root / "helpers.c"),
                    *sources, "-lcrypto", "-o", str(libpath)], check=True)
    lib = ctypes.CDLL(str(libpath))
    lib.nboot_amp_validate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.fdt_path_offset.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.fdt_open_into.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.test_setprop.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]
    lib.fdt_delprop.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
    lib.fdt_set_name.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]

    cpus = ''.join(f'cpu@{i:x} {{device_type="cpu"; compatible="arm,cortex-a72"; enable-method="psci"; reg=<0 {i}>; status="okay";}};' for i in range(0x100, 0x104))
    regions = ''.join(f'{name}@{base:x} {{reg=<0 0x{base:x} 0 0x{size:x}>; no-map;}};' for name,base,size in (
        ("rpmsg",0x47800000,0x200000),("rpmsg-dma",0x47a00000,0x200000),
        ("amp-shmem",0x47c00000,0x400000),("openvela",0x4a400000,0x1000000)))
    dts = '/dts-v1/; / { #address-cells=<2>; #size-cells=<2>; cpus {#address-cells=<2>; #size-cells=<0>;' + cpus + '};'
    dts += 'reserved-memory {#address-cells=<2>; #size-cells=<2>; ranges;' + regions + '};'
    dts += 'serial@2ad40000 {reg=<0 0x2ad40000 0 0x100>; status="disabled";};'
    dts += 'rockchip-amp {compatible="rockchip,amp"; status="okay"; amp-irqs=/bits/ 64 <108 128 0 174 128 0>;};};'
    header=bytearray(bytes(56)+b'ARMd'+bytes(8))
    struct.pack_into('<Q',header,16,0x2000)
    (root / "kernel").write_bytes(header)
    (root / "nuttx").write_bytes(header)
    (root / "ramdisk").write_bytes(gzip.compress(b'fixture'))

    def fixture(source=dts):
        (root / "test.dts").write_text(source)
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(root / "fdt"), str(root / "test.dts")], check=True, capture_output=True)
        its = '/dts-v1/; / {description="test"; #address-cells=<2>; images {'
        for name,file,kind,comp,load,cpu in (
                ('linux','kernel','kernel','none',0x42000000,0x100),
                ('fdt','fdt','flat_dt','none',0x4f000000,None),
                ('ramdisk','ramdisk','ramdisk','gzip',0x50000000,None),
                ('openvela','nuttx','firmware','none',0x4a400000,0)):
            its += f'{name} {{description="{name}"; data=/incbin/("{root/file}"); type="{kind}"; arch="arm64"; compression="{comp}"; load=<0 0x{load:x}>;'
            if cpu is not None:
                its += f'entry=<0 0x{load:x}>; cpu=<0x{cpu:x}>;'
            if name in ('linux','ramdisk'):
                its += 'os="linux";'
            its += 'hash {algo="sha256";};};'
        its += '}; configurations {default="conf"; conf {nyabula,amp-abi=<2>; kernel="linux"; fdt="fdt"; ramdisk="ramdisk"; loadables="openvela";};};};'
        (root / "test.its").write_text(its)
        subprocess.run(["mkimage","-E","-B","0x200","-p","0x1000","-f",str(root/"test.its"),str(root/"test.itb")],check=True,capture_output=True)
        return (root / "test.itb").read_bytes()

    good = fixture()
    last_end = sum(int(subprocess.check_output(
        ['fdtget','-tx',str(root/'test.itb'),'/images/openvela',key], text=True).strip(),16)
        for key in ('data-position','data-size'))
    passed = 0
    def run(name, data, ok=False, edit=None, length=None):
        global passed
        buf = ctypes.create_string_buffer(data, len(data))
        if edit:
            assert lib.fdt_open_into(buf, buf, 4096) == 0
            edit(buf)
        ret = lib.nboot_amp_validate(buf, len(data) if length is None else length)
        assert (ret == 0) == ok, (name,ret)
        passed += 1
        print(f'{name}: PASS ({ret})', flush=True)
    def prop(path, name, value):
        def edit(buf):
            node=lib.fdt_path_offset(buf,path.encode()); assert node>=0
            assert lib.test_setprop(buf,node,name.encode(),value,len(value))==0
        return edit
    def delete(path, name):
        def edit(buf):
            node=lib.fdt_path_offset(buf,path.encode()); assert node>=0
            assert lib.fdt_delprop(buf,node,name.encode())==0
        return edit

    run('valid',good,ok=True)
    for length in (0,8,40,last_end-1):
        run(f'truncated-{length}',good,length=length)
    run('optional-padding-trimmed',good,ok=True,length=last_end)
    run('corrupt-payload',good[:4096]+bytes([good[4096]^1])+good[4097:])
    for name, path, key, value in (
        ('old-cpu3','/images/openvela','cpu',struct.pack('>I',3)),
        ('wrong-linux-cpu','/images/linux','cpu',struct.pack('>I',0)),
        ('short-cpu','/images/linux','cpu',b'\0'),
        ('wrong-load','/images/openvela','load',struct.pack('>II',0,0x40200000)),
        ('header-overlap','/images/linux','data-position',struct.pack('>I',0)),
        ('out-of-bounds','/images/linux','data-position',struct.pack('>I',0x7fffffff)),
        ('zero-size','/images/linux','data-size',struct.pack('>I',0)),
        ('old-abi','/configurations/conf','nyabula,amp-abi',struct.pack('>I',1)),
        ('extra-loadable','/configurations/conf','loadables',b'openvela\0linux\0'),
        ('ignore-hash','/images/linux/hash','ignore',struct.pack('>I',1)),
        ('bad-hash','/images/linux/hash','value',bytes(32)),
    ):
        run(name,good,edit=prop(path,key,value))
    run('missing-abi',good,edit=delete('/configurations/conf','nyabula,amp-abi'))
    run('missing-hash',good,edit=delete('/images/linux/hash','value'))
    def rename(buf):
        node=lib.fdt_path_offset(buf,b'/images/linux/hash'); assert node>=0
        assert lib.fdt_set_name(buf,node,b'checksum')==0
    run('fake-hash-node',good,edit=rename)
    for name,old,new in (
        ('big-core-disabled','reg=<0 256>; status="okay"','reg=<0 256>; status="disabled"'),
        ('wrong-carveout','0x4a400000','0x4a500000'),
        ('uart-conflict','reg=<0 0x2ad40000 0 0x100>; status="disabled"','reg=<0 0x2ad40000 0 0x100>; status="okay"'),
        ('wrong-irq-route','108 128 0 174 128 0','108 128 0 174 128 3'),
        ('missing-private-irq','108 128 0 174 128 0','108 128 0 108 128 0'),
    ):
        assert old in dts
        run(name,fixture(dts.replace(old,new)))
    prefix='cpus {#address-cells=<2>; #size-cells=<0>;'
    for status in ('okay','disabled'):
        node=f'cpu@0 {{device_type="cpu"; reg=<0 0>; status="{status}"; enable-method="psci";}};'
        run('reject-small-core-'+status,fixture(dts.replace(prefix,prefix+node)))
    node='cpu@0 {reg=<0 0>; enable-method="psci"; status="disabled";};'
    run('untyped-small-core',fixture(dts.replace(prefix,prefix+node)))
    run('wrong-enable-method',fixture(dts.replace('enable-method="psci"','enable-method="spin-table"')))
    for name,file,offset,value in (
        ('linux-runtime-too-large','kernel',16,0x5000001),
        ('nuttx-runtime-too-large','nuttx',16,0x1000001),
        ('runtime-too-small','nuttx',16,32),
        ('linux-load-offset-misaligned','kernel',8,0x80000),
        ('big-endian-image','kernel',24,1),
    ):
        original=(root/file).read_bytes()
        changed=bytearray(original);struct.pack_into('<Q',changed,offset,value)
        (root/file).write_bytes(changed)
        run(name,fixture())
        (root/file).write_bytes(original)
    if len(sys.argv)>2:
        run('actual-artifact',pathlib.Path(sys.argv[2]).read_bytes(),ok=True)
    print(f'NBOOT_AMP_VALIDATION_PASS cases={passed}')

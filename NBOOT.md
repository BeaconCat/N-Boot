# N-Boot

N-Boot is the Pharos Tech downstream distribution of U-Boot for Nyabula,
KICKPI-K7, RK3576, NuttX, and openvela.

N-Boot is not a from-scratch replacement for U-Boot. It preserves U-Boot's
history, copyright notices, and applicable licenses. The `N-Boot` name
identifies the downstream product and its independently implemented board and
boot-management work.

## Clean-room boundary

The KICKPI-K7 board description, RK3576 integration, and NuttX A/B command were
implemented without copying the Android vendor U-Boot source tree. Hardware
facts and serial traces were used as interoperability evidence. The DDR
initializer, ARM Trusted Firmware, and OP-TEE binaries used during board tests
are external Rockchip components and are not relicensed by this repository.

## Implemented scope

- KICKPI-K7 device tree and minimal/full build profiles;
- native ARM64 Linux Image header accepted by the vendor BL31 chain;
- N-Boot serial identity and recovery console;
- redundant, CRC-protected boot-control records;
- independent NuttX A/B priorities, retry state, successful state, version,
  image length, and SHA-256 digest;
- verified NuttX loading at `0x40200000`;
- automatic rejection of a damaged slot and same-boot fallback;
- atomic persistence of the selected active slot;
- automatic NuttX startup through `bootnuttx 0`.

## Build profiles

```text
kickpi-k7-rk3576_defconfig
kickpi-k7-rk3576-full_defconfig
```

Both profiles use the vendor BL33 address contract at `0x40200000`. The full
profile additionally enables the broader recovery and storage command set.

Example:

```sh
make CROSS_COMPILE=aarch64-linux-gnu- kickpi-k7-rk3576-full_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j4 u-boot-nodtb.bin u-boot.dtb
```

The resulting U-Boot payload still requires board-compatible DDR, BL31, and
OP-TEE components when packed for the current KICKPI-K7 vendor boot chain.

## Verified board behavior

The following paths were verified on a 4 GiB KICKPI-K7:

- vendor SPL and BL31 load N-Boot at `0x40200000`;
- ARM64 relocation reaches the interactive `N-Boot>` console;
- both SD and eMMC controllers enumerate;
- a valid NuttX A slot passes SHA-256 verification and boots;
- a deliberately corrupted A slot is rejected;
- B boots in the same attempt and remains active after reset;
- both 4096-byte boot-control copies remain identical after the update.

## Current limitation

The board-validation FIT occupies 4 MiB because the clean KICKPI-K7 device
tree does not yet fit beside two independent 2 MiB bootloader candidates.
NuttX and AMP Linux retain independent A/B partitions, but redundant N-Boot
payloads require a smaller runtime device tree or a revised vendor-compatible
layout before being claimed as complete.

## Licensing and upstream

N-Boot follows the license of each U-Boot source file. See `Licenses/README`
and per-file SPDX identifiers. The authoritative U-Boot source repository is
maintained by the U-Boot project; the GitHub repository is used as the fork
network and collaboration mirror for this downstream.

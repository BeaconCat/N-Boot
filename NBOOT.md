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
- automatic NuttX startup from the BootROM-selected SD or eMMC medium.
- USB2 Fastboot recovery when no NuttX slot remains bootable;
- allowlisted, read-back-verified staging of NuttX A/B slots;
- standard Fastboot partition writes and erases without an unlock challenge;
- verified, read-back-checked updates of the vendor-compatible N-Boot FIT.
- persistent one-shot reboot requests and a versioned NuttX handoff record
  in PMU1 GRF scratch registers.

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
- USB Fastboot enumerates as `18d1:d00d`;
- NuttX slot B can be staged, verified, activated, and booted;
- a 4 MiB N-Boot FIT can be verified, updated, and booted again.

## Current limitation

Activated NuttX slots remain eligible regardless of the legacy retry count,
and normal boot does not consume retries. This also
recovers slots whose retries were exhausted by earlier builds. Each image
still passes SHA-256 verification before execution; a failed image has its
priority cleared and falls back to the other slot. An OS hang after a valid
image starts is not detected by this policy. The on-disk format is unchanged.

The vendor SPL checks candidates 2 MiB apart, while the interoperable FIT
layout occupies 4 MiB. N-Boot therefore uses one verified in-place update
region and does not claim power-loss atomicity for self-update. NuttX retains
independent A/B partitions. AMP A/B partitions are reserved but AMP flashing
is outside this change.

Standard Fastboot writes and erases are unrestricted for GPT-named partitions.
`fastboot flash nuttx_a|nuttx_b <image>` uses the invalidate-before-write,
media read-back, SHA-256, and bootctrl metadata update path so the resulting
slot remains bootable. Direct `boot`, `set_active`, arbitrary OEM execution,
and UUU commands remain disabled.

## System boot contract

N-Boot accepts a persistent one-shot request in the first four padding bytes
of each CRC-protected bootctrl record (offset 236, little-endian). The writer
uses the normal redundant-record update, including a new generation and CRC.
N-Boot clears the request in a new record before acting on it. Normal boots
without a request do not write metadata.

The request value is `0x4e425200 | target`, with targets `1=console`,
`2=Fastboot`, `3=NuttX A`, and `4=NuttX B`. An invalid target is consumed and
ignored. Slot requests affect one boot only and do not change `active_slot`.
The record format remains version 1; older versions ignore these padding
bytes, so both N-Boot and the OS utility must support persistent requests.

PMU1 OS_REG12 is still accepted as a legacy request source, but the tested
vendor reset chain did not preserve it reliably, including with the second
global software reset. OS_REG13 through OS_REG15 continue to carry the handoff:

| Address | Direction | Meaning |
|---|---|---|
| `0x26026230` | OS to N-Boot | Legacy one-shot reboot request |
| `0x26026234` | N-Boot to OS | Handoff header with medium and slot |
| `0x26026238` | N-Boot to OS | bootctrl generation bits 31:0 |
| `0x2602623c` | N-Boot to OS | bootctrl generation bits 63:32 |

The persistent request takes precedence over a legacy register request.

Before branching to a verified NuttX image, N-Boot writes the generation words
and then publishes the header last:

```text
bits 31:16  magic 0x4e48
bits 15:12  handoff version (2)
bits 11:8   reason: 0=normal, 1=requested slot, 2=fallback
bits 7:4    medium: 1=SD, 2=eMMC
bits 3:0    slot: 0=A, 1=B
```

N-Boot clears an old handoff header at the start of every boot. A system-side
reader must validate magic and version before using the slot or generation.
The handoff allows a later `nbootctl` service to identify the running slot and
mark it successful without guessing from partition priority.

## SD and eMMC selection

The same GPT layout is supported on SD (`mmc0`) and eMMC (`mmc1`). N-Boot
selects a valid SD layout first, then eMMC. This allows a recovery SD card to
repair eMMC and an eMMC installation to boot when the SD card is absent or
damaged, without reading the BootROM SRAM source field from U-Boot proper.

Automatic fallback requires exact `uboot`, `trust`, `bootctrl`, `nuttx_a` and
`nuttx_b` starts and sizes, so an unrelated partition table is not selected by
accident. Fastboot defaults to the selected boot medium. A recovery operator
can choose a destination for the current USB session:

```text
fastboot oem board:target:auto
fastboot oem board:target:sd
fastboot oem board:target:emmc
fastboot getvar nboot-medium
```

Changing the Fastboot target does not write media and resets on USB disconnect.
Subsequent standard Fastboot writes and erases do not require authorization.

## Early initialization

Both K7 configurations defer the full device-model scan until after
relocation, when caches and the live device tree are available. The early
board hook binds only `/dmc`, preserving the vendor loader's DRAM size
discovery without hard-coding the installed memory size. Debug UART handles
early output; the regular serial driver is initialized after relocation.
Board information and the OTP model lookup are also printed after relocation.

The configuration and the DMC hook must be kept together: skipping the early
scan without binding the RAM device prevents `dram_init()` from completing.
This does not change the SD/eMMC selection policy or enable caches early.

## Experimental four-plus-four AMP boot

The K7 full profile provides `bootamp <address> <size> [check]`. Both arguments
are hexadecimal. `check` validates the ABI2 external-data FIT without loading
payloads or starting CPUs. The default boot command remains `bootnuttx`.

The FIT contains Linux at 0x42000000, its matching K7 DTB and initramfs, and
openvela at 0x4a400000. Linux owns A72 MPIDRs 0x100..0x103; openvela owns A53
MPIDRs 0..3. The Linux DTB must contain only the four A72 CPU nodes. This uses
Rockchip BL31's nonboot-CPU Linux argument service and PSCI; it is not a generic
U-Boot boot protocol or a security boundary.

After initializing PSCI, N-Boot prepares the payloads, enables the mailbox
receiver, starts Linux on 0x100, and waits for GIC setup and the initial RPMsg
receive-buffer notification. It then enters openvela on the current CPU0.
Failure after Linux starts resets the entire SoC. Independent peer restart is
not supported. The matching openvela build needs the shared-GIC adaptation.

With the complete N-Boot vendor FIT already installed, use `fastboot usb 0`,
then host-side `fastboot stage amp.itb`. Send one ETX over serial to leave
Fastboot. Its RAM buffer is 0x60000000. Send commands with a single CR, not CRLF:

```text
bootamp 60000000 <hex-file-size> check
bootamp 60000000 <hex-file-size>
```

On 2026-09-10, hardware testing reached four-core openvela NSH and four-core
Linux, with successful real RPMsg `nyampctl health` and `nyampctl info` calls.
This does not load AMP A/B slots automatically or validate NPU workloads.
Repeated LMB reservation warnings for already reserved ranges remain visible
during bootm preparation; the tested handoff completes despite them.

Never flash this AMP FIT into ordinary NuttX slots, which boot at 0x40200000.
The bootloader itself still requires the team's complete 4 MiB vendor FIT;
do not flash the raw U-Boot proper binary into the bootloader partition.

## Licensing and upstream

N-Boot follows the license of each U-Boot source file. See `Licenses/README`
and per-file SPDX identifiers. The authoritative U-Boot source repository is
maintained by the U-Boot project; the GitHub repository is used as the fork
network and collaboration mirror for this downstream.

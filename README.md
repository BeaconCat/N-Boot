<div align="center">

# N-Boot

### 面向 Nyabula、KICKPI-K7、RK3576 与 openvela 的可靠启动管理层

**A verified, recoverable U-Boot downstream for NuttX A/B boot**

[![Upstream](https://img.shields.io/badge/upstream-U--Boot-0b7285?style=flat-square)](https://github.com/u-boot/u-boot)
[![SoC](https://img.shields.io/badge/SoC-RK3576-5c7cfa?style=flat-square)](https://www.rock-chips.com/)
[![Board](https://img.shields.io/badge/board-KICKPI--K7-f59f00?style=flat-square)](https://www.kickpi.com/)
[![OS](https://img.shields.io/badge/OS-NuttX%20%2F%20openvela-12b886?style=flat-square)](https://openvela.io/)
[![Status](https://img.shields.io/badge/status-hardware%20verified-2f9e44?style=flat-square)](#实机验证状态)
[![License](https://img.shields.io/badge/license-GPL--2.0%2B-495057?style=flat-square)](Licenses/README)

```text
     __                _             _
  /\ \ \ _   _   __ _ | |__   _   _ | |  __ _
 /  \/ /| | | | / _` || '_ \ | | | || | / _` |
/ /\  / | |_| || (_| || |_) || |_| || || (_| |
\_\ \/   \__, | \__,_||_.__/  \__,_||_| \__,_|
         |___/
   ___                _
  / __\  ___    ___  | |_
 /__\// / _ \  / _ \ | __|      N - Boot
/ \/  \| (_) || (_) || |_        RK3576
\_____/ \___/  \___/  \__|
```

</div>

> [!IMPORTANT]
> N-Boot 是 U-Boot 的 Pharos Tech downstream，并非从零重写整个 U-Boot。
> 仓库保留上游历史、版权声明和适用许可证。KICKPI-K7板级支持与NuttX A/B
> 启动管理采用clean-room方式实现，没有复制Android vendor U-Boot源码。

## 为什么需要 N-Boot

Nyabula不仅需要“把系统拉起来”，还需要在远程更新失败、镜像损坏和双系统协同
场景下继续可恢复。N-Boot在vendor SPL、ATF、OP-TEE与openvela之间增加了明确的
验证和回退边界：

```mermaid
flowchart TD
    ROM[RK3576 BootROM] --> SPL[Vendor SPL / DDR]
    SPL --> ATF[BL31 + OP-TEE]
    ATF --> NB[N-Boot]
    NB --> BC{bootctrl 双副本}
    BC -->|A 有效| A[NuttX A]
    BC -->|A 拒绝| B[NuttX B]
    BC -->|均不可启动| REC[Recovery / Fastboot 规划中]
    A --> OK[openvela / Nyabula]
    B --> OK
```

## 新增能力

| 能力 | 实现 | 状态 |
|---|---|---:|
| KICKPI-K7板级支持 | clean-room DTS、UART、SD、eMMC | 实机通过 |
| 双构建profile | minimal恢复版与full功能版 | 编译通过 |
| vendor BL33兼容 | 固定入口与链接地址`0x40200000` | 实机通过 |
| 原生ARM64 Image Header | 保持`_start`页对齐，满足BL31合同 | 实机通过 |
| N-Boot身份 | 启动画面、版本标识、`N-Boot>`控制台 | 实机通过 |
| 冗余bootctrl | 两份4096-byte记录、generation和CRC32 | 实机通过 |
| NuttX A/B选择 | active、priority、tries、successful | 实机通过 |
| 镜像完整性 | 长度、版本与SHA-256验证 | 实机通过 |
| 原子元数据更新 | 先写旧副本、回读、再写另一副本 | 实机通过 |
| 损坏槽自动回退 | 同一启动周期拒绝A并启动B | 实机通过 |
| active槽持久化 | 仅状态变化时写盘，稳定启动不磨损 | 实机通过 |
| 自动启动 | 倒计时后执行`bootnuttx 0` | 实机通过 |
| USB Fastboot救援 | DWC3/USB2 gadget分阶段接入 | 规划中 |
| NuttX/AMP统一OTA | 独立domain与非活动槽更新 | 规划中 |

## A/B元数据模型

N-Boot在`bootctrl`分区维护两个等价记录。启动时忽略magic、版本或CRC错误的
副本，并选择generation最大的有效记录。

每个系统domain独立维护：

```text
active_slot
slot A/B:
  priority
  tries_remaining
  successful
  image_size
  image_version
  sha256
```

NuttX和后续AMP Linux不会共用active状态，因此任一侧更新或回退都不会强制切换
另一侧。

### 写入顺序

```text
写入非活动槽
  → 从介质读回
  → 校验长度与SHA-256
  → 更新较旧bootctrl副本
  → 回读并校验CRC/generation
  → 更新另一副本
  → 重启尝试新槽
```

## 实机验证状态

测试平台：KICKPI-K7、RK3576、4 GiB LPDDR5、COM14 @ 1500000。

已确认完整路径：

```text
BootROM
  → DDR v1.09
  → Vendor SPL v1.08
  → BL31 v1.20
  → OP-TEE v1.06
  → N-Boot relocation
  → bootctrl
  → NuttX EL2 → EL1 → NSH / ADB
```

故障注入结果：

```text
bootnuttx: slot a rejected (-129)
bootnuttx: booting NuttX slot b, version 1
```

最终持久状态：

| 字段 | 实测值 |
|---|---:|
| generation | 12 |
| active slot | B |
| A priority | 0 |
| B priority | 14 |
| bootctrl副本 | 4096字节逐字节一致 |
| B运行态 | NSH与ADB在线 |

## 构建

### minimal恢复profile

```sh
make CROSS_COMPILE=aarch64-linux-gnu- kickpi-k7-rk3576_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j4 u-boot-nodtb.bin u-boot.dtb
```

### full功能profile

```sh
make CROSS_COMPILE=aarch64-linux-gnu- kickpi-k7-rk3576-full_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j4 u-boot-nodtb.bin u-boot.dtb
```

关键配置：

```text
CONFIG_TEXT_BASE=0x40200000
CONFIG_LINUX_KERNEL_IMAGE_HEADER=y
CONFIG_LNX_KRNL_IMG_TEXT_OFFSET_BASE=0x40000000
CONFIG_BOOTCOMMAND="bootnuttx 0"
```

> [!NOTE]
> 当前KICKPI-K7 vendor启动链仍需匹配的DDR initializer、BL31和OP-TEE外部
> 组件。这些二进制不因本仓库而重新许可。

## 分支与贡献流程

- `n-boot/main`：受保护集成分支，只接受Pull Request；
- 功能与修复：一个commit只做一件事；
- 禁止直接推送、强推或删除集成分支；
- 通用改动后续整理为可向U-Boot上游提交的独立patch；
- K7产品策略与N-Boot品牌能力保留在downstream。

## 当前限制

当前实机验证FIT为4 MiB。clean K7 DTB尚未压缩到可同时容纳两个独立2 MiB
bootloader候选，因此：

- NuttX A/B已经完成；
- AMP Linux已预留独立A/B模型；
- N-Boot自身双候选尚未宣称完成；
- 后续优先裁剪运行时DT，恢复vendor SPL固定的`0x4000/0x5000`双候选。

## 路线图

- [x] RK3576/KICKPI-K7 N-Boot启动
- [x] NuttX SHA-256验证启动
- [x] bootctrl双副本与A/B回退
- [x] active槽持久化与自动启动
- [ ] USB2 Fastboot只读枚举
- [ ] 双槽均失效时自动进入恢复模式
- [ ] 白名单Fastboot分区刷写
- [ ] N-Boot双候选更新
- [ ] NuttX/AMP Linux统一OTA manifest
- [ ] 签名验证与anti-rollback

## 上游、版权与许可证

N-Boot基于[Das U-Boot](https://github.com/u-boot/u-boot)。原始U-Boot说明完整
保留在[`README`](README)，详细的N-Boot边界说明见[`NBOOT.md`](NBOOT.md)。

本仓库遵循每个源文件的SPDX标识和适用许可证。完整许可说明位于
[`Licenses/README`](Licenses/README)。U-Boot原作者、贡献者版权和Git历史均予以
保留；Pharos Tech仅对其新增实现主张相应版权。

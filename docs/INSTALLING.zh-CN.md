# 安装与恢复 UrsusBoot

这份文档面向实际操作。多数情况下推荐使用 **[UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher)**：它负责型号识别、传输通道选择、备份、预检、写后读回与支持的恢复流程。

[中文 README](../README.zh-CN.md) · [English original](INSTALLING.md)

## 选择哪个构建文件？

一次成功的持久安装构建可能生成：

- `u-boot.bin`：由当前源码生成的原始 U-Boot，通常不直接刷入；
- `u-boot.lzma`：由上述文件压缩得到的 Airoha LZMA1EXT/no-EOPM BL33；
- `ursusboot-update.fip`：将参考容器中的 NT_FW/BL33 替换为本次构建后的可刷写 FIP；
- `ursusboot-install-mtd0.bin`：板卡模板加新 FIP 组合而成的完整 **512 KiB** 引导区镜像；
- `SHA256SUMS`：本次构建产物的 SHA256 校验和。

**参考 FIP 与输出 FIP 不是同一文件。** `URSUS_FIP_DONOR`、`URSUS_FIP_TEMPLATE` 和旧名 `URSUS_FIP` 都表示参考容器；构建流程会用当前 `u-boot.lzma` 替换其中的 BL33。

## 推荐路线：UrsusFlasher

根据设备当前状态，UrsusFlasher 可从 UrsusBoot WebFailsafe、Nokia 原厂 root Linux、OpenWrt，或严重变砖时的 Airoha UART/BootROM 路线执行操作。请把本次构建的 `ursusboot-update.fip` 或板卡正确的安装镜像交给程序，按其工作流完成预检、传输、写入和读回。详见 [UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) 及[中文操作说明](https://github.com/Medvedolog/airoha-router-ursusflasher/blob/main/docs/INSTRUCTIONS_ZH.md)。

**普通 UrsusBoot Recovery：**在通电或重启**之后**，整机 LED 重新启动时按住 Reset；经历 2 次短闪、3 次长闪，红灯常亮时松开，再打开 `http://192.168.1.1`。**通电之前**按住 Reset 进入的是 BootROM，而不是普通 WebFailsafe。

## 恢复网络的信任边界

WebFailsafe 是**物理恢复界面**，不是日常远程管理服务：

- HTTP 没有认证，也没有 TLS；
- WebFailsafe 运行时会监听所有 IPv4 接口；
- `/api/console` 暴露真实的 U-Boot 命令行；
- 满足确认及前置条件后，刷写/迁移端点可以执行写入。

恢复时让电脑与路由器通过可信、隔离的网线直连。不要把 WebFailsafe 接到共享 LAN、上联、Wi-Fi 桥接或其他不可信的二层网络。

## 路线 A：具有 root 权限的 Nokia 原厂 Linux

写入之前先确认引导 MTD 的几何信息：

```sh
cat /proc/mtd
cat /sys/class/mtd/mtd0/bad_blocks
```

在支持的 Nokia 512 KiB 引导区路线中，`mtd0` 必须为引导程序分区，并满足：

```text
size       00080000
erase size 00020000
bad_blocks 0
```

备份整个引导区：

```sh
dd if=/dev/mtd0 bs=131072 count=4 of=/tmp/mtd0-backup.bin
sha256sum /tmp/mtd0-backup.bin
```

把备份复制到电脑，在电脑上核对相同 SHA256。仅存放于路由器 `/tmp` 的文件不能算可用于恢复的备份。将 `ursusboot-install-mtd0.bin` 传回路由器，写入前也要验证它的哈希。

**一次只选一个写入器。** 写入器开始工作后可能已经擦除了闪存块，不能仅因为输出看起来不寻常就换另一个写入器。下列是不同环境可能使用的方案，**不是要依次执行的步骤**：

```sh
mtd write /tmp/ursusboot-mtd0.bin bootloader
```

或：

```sh
flash_erase /dev/mtd0 0 0
nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin
```

或：

```sh
flash_eraseall /dev/mtd0
nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin
```

或：

```sh
mtd_debug erase /dev/mtd0 0 0x80000
mtd_debug write /dev/mtd0 0 0x80000 /tmp/ursusboot-mtd0.bin
```

写入后：

```sh
sync
dd if=/dev/mtd0 bs=131072 count=4 2>/dev/null | sha256sum
```

读回的 SHA256 必须与安装镜像相同，之后才能重启。如果不符，**保持当前系统运行，不要断电**，趁恢复通道还在时排查写入。部分 OpenWrt 内核将引导 MTD 标为只读；UrsusFlasher 针对此情况提供固定的 `mtd-rw` 路线和范围受限的 `ursus-mtd-raw` 写入器。

## 路线 B：通过 UART 访问正在运行的 U-Boot

使用 **3.3 V** USB-UART：只连接 **TX / RX / GND**，不要接 VCC；设置 **115200、8N1、无流控**。

在 U-Boot 提示符先查询实际分区名称：

```text
mtd list
loadx 0x8e000000
```

通过 XMODEM 发送 `ursusboot-install-mtd0.bin`，随后写入 `mtd list` 显示的实际引导分区。原厂布局通常叫 `bl2`，规范 UrsusBoot/OpenWrt UBI 布局可能叫 `ursus-ubi-bl2`。**以下仅是名称为 `bl2` 时的示例；不能猜测分区名：**

```text
mtd erase bl2 0x0 0x80000
mtd write bl2 0x8e000000 0x0 0x80000
```

## 路线 C：完全变砖 / Airoha BootROM

当 U-Boot 提示符也不可用时，已验证的恢复过程分两阶段：

1. 断电；
2. **通电前**按住 Reset，然后接通电源；
3. 通过 XMODEM 发送已验证的 Airoha preloader/BL2；
4. 再通过 XMODEM 发送已验证的 RAM-installer FIP；
5. 在 RAM 中运行 U-Boot，此时仅加载代码尚未修改 NAND；
6. 经正常的验证和读回路线安装持久 UrsusBoot。

本仓库的 `ram-recovery` 角色设置 `bootcmd=ursusweb;true`，但**这本身不能证明**任意重新打包的 FIP 都满足实机验证过的 BootROM 第二阶段容器/入口要求。在完成实机验证前，砖机恢复应使用 UrsusFlasher 固定的、已验证的 RAM-installer。之后再通过 WebFailsafe/UrsusFlasher 安装新编译的持久 FIP。

## 恢复环境中的 MAC 地址

UrsusBoot 每次启动都会在 `preboot` 阶段尝试从 `ri` 卷刷新原厂 MAC；重置环境变量后，`reset_factory` 也会重新推导并保存地址。

U-Boot 在 `preboot` **之前**探测以太网。因此环境为空时可能暂时显示：

```text
Warning: ... using random MAC address - ...
```

这条警告本身不代表恢复失败。应进一步检查随后出现的：

```text
URSUS_MAC_SOURCE=RI ethaddr=...
```

保留随机 MAC 作为后备机制，是为了在 `ri` 受损时仍能通过网络进入恢复环境。

## 如何判断成功

- **QA PASS**：仓库结构与自检通过；
- **build PASS**：目标 U-Boot 与打包流程成功编译；
- **HW PASS**：**该确切产物**在目标设备上按记录的恢复路线和验收项目完成实机测试。

不要把 CI/构建结果当作实机通过。

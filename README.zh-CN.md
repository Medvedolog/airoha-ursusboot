# UrsusBoot

[English](README.md) · [Русский](README.ru.md) · [中文安装与恢复说明](docs/INSTALLING.zh-CN.md)

**面向 Airoha 路由器的完整 U-Boot 引导程序，内置 OpenWrt 安装与恢复环境。**

UrsusBoot 是“熊家族”中运行在路由器里的小熊：它可与电脑端 [UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) 配合，也可以独立通过 WebFailsafe 安装或恢复 OpenWrt。项目基于 **U-Boot 2026.07**、OpenWrt/Airoha 平台补丁及自身的恢复层；不是从零编写的新引导程序。

## 用途与能力

在约 512 KiB 的引导区约束下，UrsusBoot 提供无需启动 Linux 即可使用的恢复环境：

- 内置 WebFailsafe 网页、HTTP API 和**真正的 U-Boot 控制台**；
- 识别及严格校验 OpenWrt sysupgrade/FIT 镜像，并报告具体拒绝原因；
- 识别 Nokia 原厂、OpenWrt 原厂布局及规范 UBI 布局；
- 创建 UBI、迁移布局、安装/更新 OpenWrt；
- 验证写入内容的读回结果；独立更新 FIP/引导程序；
- 从 RAM 启动 initramfs，读取 NAND/UBI 状态和坏块信息；
- 通过 UART/Airoha BootROM 恢复无法正常启动的设备。

U-Boot 原有的 MTD/UBI、网络、环境变量和启动命令仍可使用。为了符合大小限制，项目关闭了此路由器恢复流程不使用的文件系统、存储总线等模块。**FIT 哈希会校验；FIT RSA 签名验证未启用。**网页的“U-Boot 控制台”执行真实命令，不受普通网页按钮的确认保护。

## 与 UrsusFlasher 的关系

| 项目 | 所在位置及职责 |
|---|---|
| **UrsusBoot**（本仓库） | 路由器中的引导与恢复环境；可单独使用网页安装、更新和恢复 |
| [UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) | 电脑端的安装、备份、状态识别、传输和读回验证工具；可调用 UrsusBoot |
| [UrsidoRescue](https://github.com/Medvedolog/airoha-ursidorescue) | Web/SSH/引导程序不可用时的独立 USB-UART / BootROM 救援工具 |

UrsusFlasher 会固定本仓库的精确构建版本，以便工具包中的 MD/MF 引导程序与元数据保持一致。Vanilla U-Boot 是最终 UBI 布局的另一条产品路线，不等同于 UrsusBoot。

## 支持范围与工作方式

当前主要板卡为 Nokia XG-040G-MD（AN7581）和 XG-040G-MF（AN7583）。实际支持与实机验证状态请分别参阅本仓库记录及 [UrsusFlasher 发布说明](https://github.com/Medvedolog/airoha-router-ursusflasher/releases)。构建/CI 通过不能代替目标设备上的测试。

在规范 UBI 布局中，BL2 位于闪存起始处，UrsusBoot 的 FIP 位于 UBI `fip` 卷，OpenWrt 则使用 `fit` 和 `rootfs_data`。普通 OpenWrt sysupgrade 不会顺带覆盖 `fip`；更新 UrsusBoot 是单独的显式操作。

启动分派器会检查实际 NAND 布局，决定启动 OpenWrt 或原厂系统；启动路径返回时会回到 WebFailsafe，而不是停在失效的提示符。

## 进入网页恢复模式

正常通电或重启后，等待整机 LED 重新启动，**此时**按住 Reset，经过 **2 次短闪 + 3 次长闪**，红灯常亮时松开，然后在直连电脑上打开 `http://192.168.1.1`。

**在通电之前**按住 Reset 会进入 Airoha BootROM；这与普通 UrsusBoot Recovery 是不同的路线。

WebFailsafe 可检查固件、安装或更新 OpenWrt、迁移到 UBI、重置 OpenWrt 设置、校验和更新 UrsusBoot FIP、单次从 RAM 启动 FIT。页面只显示当前布局与镜像条件允许的控件。写入操作先校验并等待确认，再在主循环中执行，随后读回验证；完成后的重启是单独操作。

> [!WARNING]
> WebFailsafe 的 HTTP 没有登录认证或 TLS，网页的真实 U-Boot 控制台也可执行写入命令。恢复时使用电脑与路由器之间可信、隔离的直连网线，不要接到共享局域网、上联或 Wi-Fi 桥接。

## 安装与备份

**推荐使用 [UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher)**：它识别型号和状态，选择现有 WebFailsafe、Nokia 原厂 root 环境、OpenWrt 或 UART/BootROM 通道，完成备份、预检、写入和读回。需要手动操作时，请阅读[中文安装与恢复说明](docs/INSTALLING.zh-CN.md)。

UrsusBoot 可以修复引导和重装系统，但**不能重新创造丢失的原厂 MAC、序列号或光学校准数据**。操作前保留本机的可靠备份。若读回结果不符，不要立刻断电或盲目切换写入器。

构建产物的含义：

| 文件 | 用途 |
|---|---|
| `u-boot.bin` | 当前源码编译的原始 U-Boot；通常不直接刷写 |
| `u-boot.lzma` | Airoha 使用的压缩 BL33 |
| `ursusboot-update.fip` | 更新用 FIP：当前 BL33 已装入经过验证的容器 |
| `ursusboot-install-mtd0.bin` | 板卡对应的完整 512 KiB 引导区镜像 |
| `SHA256SUMS` | 本次构建产物的校验和 |

`URSUS_FIP_DONOR` 等名称指的是**参考容器**，不是不加修改就要刷入的构建结果。更多构建/移植细节暂请阅读[英文原版](README.md)和 [BUILDING.md](docs/BUILDING.md)、[PORTING.md](docs/PORTING.md)。

## 文档

- [中文安装与恢复说明](docs/INSTALLING.zh-CN.md)
- [英文 README](README.md) · [俄文 README](README.ru.md)
- [英文安装说明](docs/INSTALLING.md)
- [版本记录](CHANGELOG.md) · [测试要求](docs/TESTING.md)
- [UrsusFlasher 中文 README](https://github.com/Medvedolog/airoha-router-ursusflasher/blob/main/docs/README_ZH.md)

此中文 README 介绍用户操作和项目关系；构建、底层 API、板卡移植和开发历史仍以英文原版文档为准。

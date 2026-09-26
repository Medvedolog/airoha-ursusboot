# UrsusBoot

[English](README.md) · [Русский](README.ru.md) · [中文安装与恢复说明](docs/INSTALLING.zh-CN.md)

**面向 Airoha 路由器的紧凑型 U-Boot，内置 OpenWrt 安装与恢复能力。**

UrsusBoot 源自 OpenWrt 的 U-Boot 生态，面向特别需要常驻恢复环境的 Airoha 设备。它在有限的引导区空间内集成 WebFailsafe、OpenWrt 镜像识别、UBI 迁移与恢复功能。它是“熊家族”中运行在路由器里的完整小熊：既可与电脑端 UrsusFlasher 配合，也能独立工作。

## 为什么需要 UrsusBoot

它的核心价值是：**在约 512 KiB 引导区的限制下，提供通常由更大型恢复环境承担的大部分功能。** 校验镜像、迁移布局、安装 OpenWrt 时，不必先启动 Linux 内核和用户空间。

主要能力包括：

- WebFailsafe 恢复网页；
- 识别 OpenWrt `sysupgrade` / FIT；
- 检测 Nokia 原厂、OpenWrt 原厂布局及规范 UBI 布局；
- 创建 UBI 并迁移布局；
- 更新 FIP 和引导程序；
- 写后读回验证，条件不明时拒绝写入；
- 经网络和 Airoha BootROM/UART 进行恢复。

## UrsusBoot 究竟是什么

UrsusBoot **不是从零重写的自制引导程序**。它基于 OpenWrt 为 Airoha 构建的标准 U-Boot：上游 U-Boot 2026.07 加 OpenWrt/Airoha 平台补丁，按路由器需要裁剪配置，再叠加恢复层。熟悉 U-Boot 的用户会认得它的命令和工作方式。

**Airoha 所需的功能仍在。** 构建保留 65 个原生 U-Boot 命令，包括完整的 `mtd`/`ubi` 与环境变量功能、网络命令（`ping`、`tftpboot`、`wget`、`dhcp`、`dns`、`sntp`、`mii`、`mdio`）、启动命令（`bootm`、`booti`、`bootflow`、`bootd`、`go`、`elf`）、分区命令（`part`、`gpt`）、哈希与 CRC、LZMA/LZ4/gzip 解压，以及 `gpio`、`pinmux`、`led`、`button`、`smc`。再加七个 UrsusBoot 命令，运行中的引导程序共有 72 个命令和完全开放的控制台。

关闭的是 NAND 路由器恢复路线不使用的部分：95 组命令及子系统，包括 ext4、FAT、btrfs、squashfs、erofs、UBIFS、exFAT、cramfs、ZFS 文件系统；USB、MMC/SD、SATA、SCSI、NVMe、PCI、IDE、并行 SPI flash、OneNAND 等存储路径；EFI、视频、bootmenu/bootstd/bootmeth、TPM、i2c、ADC、DFU、fuse、pstore 和演示/基准测试命令。**FIT 哈希会校验，但 FIT RSA 签名验证（`FIT_SIGNATURE`）未启用。**体积来自裁剪配置，而非重造 U-Boot。

在标准 U-Boot 之上，UrsusBoot 增加了：

- **带真实 U-Boot 控制台的 WebFailsafe**：浏览器控制台执行完整命令，而非受限的命令子集；
- **理解 OpenWrt 的镜像流程**：先在 RAM 中接收、分类并严格校验 sysupgrade/FIT，再考虑写入；
- **UBI 创建、更新与迁移**：写后读回，并返回明确的拒绝原因；
- **启动分派器**：根据 NAND 的实际状态选择启动路径，失败时回到恢复环境；
- **FIP/引导程序更新**和 Airoha BootROM/UART 救援路径。

## 仓库的自包含原则

本仓库力求**自包含**：正常构建不应从其他项目克隆或下载源码、板卡模板、二进制参考容器。

目标构建允许的外部依赖是相应 AArch64 目标的**官方 OpenWrt SDK/工具链**；主机还需要 C 编译器和 liblzma 开发头文件，用于已验证的 BL33 LZMA1EXT/no-EOPM 压缩器。板卡模板、引导区模板、FIP 参考输入、补丁、配置及构建脚本均在本仓库。

包含的 Nokia XG-040G-MD/MF 模板是 512 KiB 原厂引导区模板，**设备身份不来自模板**。MAC、序列号、GPON 等身份数据属于 RI/BOSA/设备身份处理路线，必须单独处理。

## 不要求用户先提供 donor dump

受支持板卡的构建和安装流程不要求用户先读取自己的 `mtd0`：

```text
板卡原厂模板 + UrsusBoot FIP
              ↓
     可安装的 512 KiB mtd0 镜像
```

例如：

```bash
OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md persistent
```

输出位于 `dist/xg040-md/`：`u-boot.bin`、`u-boot.lzma`、重新打包的 `ursusboot-update.fip` 和 `ursusboot-install-mtd0.bin`。参考 FIP 提供经过验证的平台容器谱系；构建以本次 U-Boot 替换其中的 BL33，并在生成安装镜像前逐字节验证。

完全变砖的设备也可在 BootROM/UART 路线使用同一板卡模板。仍在运行 Nokia 原厂固件时，电脑端安装器可通过获得 root 的系统写入对应板卡镜像并读回检查。如有必要，可依据本机标签或设备身份来源单独恢复/配置 MAC 等身份。

## 仓库结构

```text
build.sh                  统一构建入口
scripts/                  主机端构建与 QA 辅助脚本
config/                   Kconfig 片段、完整配置、板卡配置
boards/                   板卡策略头文件及 512 KiB 原厂引导区模板
reference/                重新打包 BL33 所用的已验证参考 FIP 谱系
src/u-boot/               完整 U-Boot 2026.07 源码及 UrsusBoot 修改
dist/                     构建输出（Git 忽略）
```

`src/u-boot/` 内的 UrsusBoot 修改有意保持局部化：

```text
cmd/ursusdispatch.c       启动分派器
cmd/ursusweb.c            WebFailsafe HTTP 服务、校验、RAM 暂存
cmd/ursusubi.c            UBI 更新/迁移
cmd/ursusupdate.c         FIP 校验与更新
cmd/ursusstock.c          原厂系统桥接启动
cmd/ursusled.c            LAN/状态 LED 策略
include/ursusweb_ui.inc   内嵌网页（HTML/CSS/JS）
include/ursus_*.h         共用头文件
include/ursus_logo.inc    内嵌标志和 favicon
drivers/gpio/ursus_an7581_safe_gpio.c
defenvs/                  各板卡默认环境
```

## 与 UrsusFlasher 和 Vanilla U-Boot 的关系

- **airoha-ursusboot**（本仓库）：UrsusBoot 固件的版本和构建来源，包含 MD/MF 板卡配置、快速 BL2/preloader 来源与构建产物。
- **[airoha-router-ursusflasher](https://github.com/Medvedolog/airoha-router-ursusflasher)**：电脑端编排器，负责备份、传输、读回、诊断、OpenWrt 镜像以及 ONE-CLICK/EXPERT 工具包。它固定本仓库的精确提交；MD 与 MF 产物若不是同一提交构建，工具包会拒绝构建。
- **Vanilla U-Boot**：最终 UBI 布局使用的普通 OpenWrt U-Boot，是另一产品路线，并非由此框架构建。

UrsusBoot 在路由器中可以独立运行；UrsusFlasher 是围绕它提供操作流程、备份和救援的电脑端工具。

## 文档

操作细节位于 `docs/`：

- [中文安装与恢复说明](docs/INSTALLING.zh-CN.md) / [英文原版](docs/INSTALLING.md)：UrsusFlasher、原厂 root、UART 和 BootROM 路线；
- [BUILDING.md](docs/BUILDING.md)：工具链、板卡配置、参考 FIP 打包及产物；
- [TESTING.md](docs/TESTING.md)：QA、构建、实机验收标准，包括 MAC 和共享网络接口检查；
- [PORTING.md](docs/PORTING.md)：移植契约和板卡配置，目前保留英文；
- [PROVENANCE.md](docs/PROVENANCE.md)：独立仓库来源与二进制/容器谱系；
- [CREDITS.md](docs/CREDITS.md)：上游和社区致谢；
- [CHANGELOG.md](CHANGELOG.md)：源码、构建和实机里程碑及其证据级别。

## 主机端脚本

| 脚本 | 用途 |
|---|---|
| `build.sh` | 按板卡构建 U-Boot，将新 BL33 打包到参考 FIP，生成可安装的 `mtd0` 镜像。 |
| `src/u-boot/repack_persistent_fip.py` | 保留已验证参考 FIP 的其余内容，只替换 BL33。 |
| `src/u-boot/lzma1ext_noeopm.c` | Airoha BL33 所需的 LZMA1EXT/no-EOPM 主机端压缩器。 |
| `scripts/make-install-mtd0.py` | 合并 512 KiB 板卡引导区模板与新 FIP。 |
| `scripts/ci/build-release.sh` | 发布构建：官方 OpenWrt 快速扫描 BL2 → preloader FIP → 与其绑定的 UrsusBoot → 打包 → `PROVENANCE.json`；CI 和 UrsusFlasher 使用精确提交。 |
| `scripts/pin_ubi_preloader.py` | 将指定 UBI preloader 及 128 KiB BL2 候选的 SHA256 编入 UrsusBoot。 |
| `scripts/atf/` | ATF UBI 快速扫描补丁、OpenWrt `Build/Prepare` 钩子和 BL2→preloader FIP 包装。 |
| `scripts/mf/` | XG-040G-MF 运行时派生与 MedveFlasher 谱系 FIP 重打包。 |
| `scripts/qa.sh` | 离线 QA：模板和参考 FIP 逐字节检查、板卡配置校验、Python 辅助脚本编译及源码完整性检查。 |
| `scripts/resolve_board_profile.py` | 从 `config/board-profiles.json` 读取板卡字段。 |
| `scripts/apply_kconfig_fragment.py` | 将 Kconfig `.cfg` 片段应用至 `.config`。 |
| `scripts/apply_board_policy.py` | 应用板卡策略头文件和源码锚点。 |
| `scripts/apply_runtime_role.py` | 在 `persistent` 与 `ram-recovery` 默认环境之间切换。 |

构建示例：

```bash
OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md
URSUS_FIP_DONOR=reference/md/ursusboot-test61-update.fip \
  OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md

OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md ram-recovery
```

`OPENWRT_SDK`（或第三个位置参数）可指向解压后的 OpenWrt SDK 或独立 AArch64 工具链；脚本自动定位交叉编译器。输出在 `dist/<board>/`。`config/board-profiles.json` 定义板卡、配置、模板和允许运行的角色；未知板卡或角色、尚缺可构建配置的板卡都会明确报错。

### 参考 FIP 的语义

传给 `build.sh` 的 FIP **并不是原样复制到输出的现成镜像**。它是已验证的 Airoha 参考容器，提供 BL31、其余 FIP 条目、顺序、布局、证书/校验结构和板卡容器容量。构建始终把新编译的 `u-boot.bin` 压缩为 Airoha LZMA1EXT/no-EOPM，只替换参考容器中的 NT_FW/BL33：

```text
参考 FIP + 本次构建的 u-boot.bin
          ↓
 LZMA1EXT/no-EOPM BL33
          ↓
 仅替换 NT_FW/BL33 并重打包
          ↓
 dist/<board>/ursusboot-update.fip
```

```bash
URSUS_FIP_DONOR=/path/to/proven-donor.fip \
  OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md persistent
```

| 变量 | 含义 |
|---|---|
| `URSUS_FIP_DONOR` | 标准名称：覆盖板卡配置中的 `reference_fip`。 |
| `URSUS_FIP_TEMPLATE` | 同义兼容名称，仍表示参考容器。 |
| `URSUS_FIP` | 历史兼容名称，同样表示参考容器，绝不会原样复制/刷写。 |

未指定时使用板卡配置的 `reference_fip`。生成的 FIP 在产出 512 KiB 安装镜像前，会检查其 NT_FW/BL33 是否与本次 `u-boot.lzma` 逐字节一致。每次构建在 `dist/<board>/SHA256SUMS` 写入该次产物的哈希；仓库根目录有意不维护整树校验清单。

角色在构建工作副本 `work/<board>-<role>/u-boot` 的源码层应用；`build.sh` 不修改 `src/u-boot`，可从同一检出连续构建不同板卡和角色。细节见 [BUILDING.md](docs/BUILDING.md)。

| 角色 | `bootcmd` | 用途 |
|---|---|---|
| `persistent` | `ursusdispatch` | 闪存常驻的正常启动与恢复分派。 |
| `ram-recovery` | `ursusweb;true` | 仅在 RAM 中运行，直接进入网页恢复，不触碰常规启动路径。 |

## 安装或更新 UrsusBoot

**推荐通过 [UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) 刷写新构建的 `ursusboot-update.fip`。** Windows/Linux 电脑端工具已经实现了对应传输和验证流程：

- **UrsusBoot WebFailsafe**：网页上传新 FIP，校验、写入并读回；
- **Nokia 原厂 Linux/root**：经原厂系统传输、校验，选择合适 MTD 写入器，重启前完整读回；
- **UART / Airoha Brick Mode**：网页及原厂系统不可用时，经串口、BootROM/XMODEM 恢复。

手工路线供专业操作和后备使用；完整步骤见[中文安装说明](docs/INSTALLING.zh-CN.md)。

### 路线 A：原厂 Linux 的 root shell

写入前确认 `mtd0` 是大小 `00080000`、擦除块 `00020000`、坏块数为 0 的引导区：

```sh
cat /proc/mtd
cat /sys/class/mtd/mtd0/bad_blocks
command -v dd sha256sum
```

备份整个 512 KiB 引导区，复制到电脑并核对相同的 SHA256；只留在路由器 `/tmp` 中不算恢复备份：

```sh
dd if=/dev/mtd0 bs=131072 count=4 of=/tmp/mtd0-backup.bin
sha256sum /tmp/mtd0-backup.bin
```

将 `ursusboot-install-mtd0.bin` 传入路由器并保存为下例使用的 `/tmp/ursusboot-mtd0.bin`，写入前先核对 SHA256。**只能选择一个写入器；第一个开始后可能已经擦除块，不能因输出异常就尝试另一个。**以下是互斥的备选命令：

```sh
mtd write /tmp/ursusboot-mtd0.bin bootloader

# 或
flash_erase /dev/mtd0 0 0 && nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin

# 或
flash_eraseall /dev/mtd0 && nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin

# 或
mtd_debug erase /dev/mtd0 0 0x80000 && \
mtd_debug write /dev/mtd0 0 0x80000 /tmp/ursusboot-mtd0.bin

sync
```

重启前完整读回并比较安装镜像的 SHA256：

```sh
dd if=/dev/mtd0 bs=131072 count=4 2>/dev/null | sha256sum
```

若不一致，**不要断电**；保持当前系统运行，以便处理写入。OpenWrt 上引导 MTD 可能是只读，UrsusFlasher 为此提供固定的 `mtd-rw` 路线和范围受限的 `ursus-mtd-raw` 写入器。

### 路线 B：UART / XMODEM

使用 **3.3 V** 串口转接器，仅连接 TX/RX/GND，`115200 8N1`、无流控。有 U-Boot 提示符时，先查看分区：

```text
mtd list
loadx 0x8e000000
# 用 XMODEM 发送 ursusboot-install-mtd0.bin

mtd erase bl2 0x0 0x80000
mtd write bl2 0x8e000000 0x0 0x80000
```

写入命令中的 `bl2` **只是示例**。原厂布局通常是 `bl2`，规范 UBI 布局可能是 `ursus-ubi-bl2`；实际名称必须以 `mtd list` 为准。

完全没有提示符时，Airoha BootROM 是两阶段：断电，**通电前**按住 Reset，经 XMODEM 依次发送已验证的 preloader/BL2 和 RAM-installer FIP，然后在 RAM 中运行 U-Boot；此时尚未写 NAND。UrsusFlasher 固定的 alpha3 RAM-installer FIP 是目前已实机验证的 MD BootROM 第二阶段产物。`ram-recovery` 的 `bootcmd=ursusweb;true` 本身**不能证明**任意重新打包 FIP 符合 BootROM 第二阶段的容器/入口契约。在实机验证前，应使用已验证的 RAM-installer 救砖。随后可经 WebFailsafe/UrsusFlasher 安装持久 FIP。

## 内置命令

UrsusBoot 增加七个普通 U-Boot 控制台命令，可从 UART 输入或在环境变量中组合：

| 命令 | 参数 | 功能 |
|---|---|---|
| `ursusdispatch` | — | 启动策略。启动时检测 Reset；超过消抖时间则进入 WebFailsafe。否则按 NAND 实际状态选择 UBI 的 `ursusubiboot`、原厂内核的 `mtd read`+`bootm` 或 `ursusstockboot`。返回的路径最终进入 WebFailsafe，不停在无效提示符。 |
| `ursusweb` | — | 启动 Ethernet/lwIP、`192.168.1.1:80` 的网页及 HTTP API，同时轮询 UART shell；重复进入不重启服务。 |
| `ursusubiboot` | — | 从 UBI `fit` 卷启动 OpenWrt。 |
| `ursusstockboot` | `[master\|slave]` | 按 Nokia `tcboot` 板卡参数约定启动原厂系统。 |
| `ursusupdate` | `check\|write <addr> <len>` | 校验并可选择写入 RAM 中暂存的 FIP。 |
| `ursussettings` | `reset` | 只擦除并重建 OpenWrt UBI/原厂布局上的 `rootfs_data`；不碰固件和引导程序。 |
| `ursuslanled` | `status\|enable` | Nokia LAN2–LAN4 PHY LED 硬件路由。 |

### 环境脚本

`defenvs/<soc>_<board>_env` 保存默认环境：

| 变量 | 用途 |
|---|---|
| `bootcmd` | `ursusdispatch`。 |
| `boot_tftp`、`boot_tftp_forever` | 从网络启动恢复镜像，可循环重试。 |
| `boot_tftp_write_fip`、`boot_tftp_write_bl2` | 经 TFTP 获取并写入引导程序。 |
| `ubi_write_production`、`ubi_read_production` | 写/读 OpenWrt `fit` 卷。 |
| `ubi_write_fip`、`ubi_create_env`、`ubi_format` | 引导区 UBI 卷管理。 |
| `ethaddr_factory` | 每次启动从 `ri` 更新 MAC 并记录来源；RI 无效时退回已保存 MAC，最后才用随机恢复 MAC。 |
| `reset_factory` | 从编译默认值重建两份环境，从 RI 恢复 MAC 并保存冗余环境。 |

WebFailsafe 占用 Ethernet 时，`ping` 和 `tftpboot` 借用现有 lwIP 网络接口，不重置网络；其他网络命令会拒绝执行。在 UART 中按 `Ctrl-C` 停止 WebFailsafe，再输入 `ursusweb` 可重新启动。

## 恢复环境的安全边界

WebFailsafe 是**物理恢复界面，不是日常管理平台**：

- HTTP 无用户认证、无 TLS；
- 运行时监听所有 IPv4 接口；
- `/api/console` 暴露完整、未锁定的 U-Boot 命令行；
- 相关 API 在满足常规确认与前置条件后可暂存并写入固件、UBI 和引导程序。

应使用恢复电脑和路由器之间**可信、隔离的直连网线**。不要接共享 LAN、交换网络、Wi-Fi 桥接、ISP 上联或其他不可信二层网络。能访问 WebFailsafe 的人应被视为拥有接近实体 UART 的恢复权限。这是约 512 KiB 引导程序在确定性救砖能力与账户认证机制之间的有意取舍。

## 恢复模式的 MAC 身份

`ethaddr_factory` 在**每次**启动的 `preboot` 阶段从板卡 `ri` 卷推导原厂 MAC；`reset_factory` 清空环境后也会重新推导并保存。U-Boot 初始化以太网发生在 `preboot` **之前**：

```text
board_r.c  INITCALL(initr_net)      -> eth probe -> eth_post_probe()
board_r.c  INITCALL(run_main_loop)  -> main_loop() -> preboot -> ethaddr_factory
```

已有 `ethaddr` 时会直接采用。环境为空时，`eth_post_probe()` 先生成随机 MAC，写入环境及控制器，并可能打印 `Warning: <dev> (ethN) using random MAC address - <mac>`；随后 `ethaddr_factory` 才从 RI 修正环境。因此单独看到随机 MAC 警告不说明恢复失败，应继续寻找 `URSUS_MAC_SOURCE=RI ethaddr=…`。随机地址是本地管理地址（首字节 `0x02` 位为 1，形式为 `x2:`、`x6:`、`xa:`、`xe:`），可与原厂地址区分。

保留随机后备是为了 RI 损坏的设备仍能通过网络进入 WebFailsafe；若完全缺少 MAC，网络初始化可能因 `No valid MAC address found` 失败。`ethaddr_factory` 的 `WARN:` 会说明后备来源。两个板卡配置均启用 `CONFIG_ENV_OVERWRITE=y`；否则 `ethaddr` 的一次写入标志会拒绝后续纠正。QA 对两板均作此检查。

## Web 界面

WebFailsafe 从闪存提供独立完整的网页，不依赖 CDN 或外部资源；界面本身支持俄语/英语切换。

![Nokia XG-040G-MD 完成 STOCK→UBI 迁移后的 UrsusBoot WebFailsafe](docs/images/webfailsafe-xg040g-md-t71.webp)

*图：Nokia XG-040G-MD（AN7581、SkyHigh SPI-NAND）在 UrsusFlasher ONE-KEY 完成 Nokia STOCK→OpenWrt UBI 迁移后的 UrsusBoot 0.1.0-alpha5-t71。各迁移阶段通过，诊断显示迁移验证后的卷；点击“重启进入 OpenWrt”后，打开的网页可识别路由器已离开 UrsusBoot。*

左侧是固件安装，右侧显示只读状态及重启控件；标签页提供验证日志、真实 U-Boot 控制台、单次 initramfs/FIT 启动和 UrsusBoot 更新。状态由 `GET /api/status` 轮询，日志来自 `GET /api/log` 与 `GET /api/operation-log`。

### 各按钮的作用

所有可能写入的按钮都需要明确的确认头。**HTTP 处理器本身不写 NAND**：它先验证并登记待执行操作，答复客户端，再由 `ursusweb` 主循环写入并报告进度。重启始终是独立的手动操作。

| 按钮 | 端点 | 确认值 | 前置条件 | 效果 |
|---|---|---|---|---|
| 检查固件 | `POST /api/firmware-begin` + `…/firmware-chunk` | — | — | 在 RAM 暂存并分类；只读。 |
| 移除文件 | `POST /api/discard` | — | — | 清除暂存镜像及元数据。 |
| 安装 OpenWrt | `POST /api/install-openwrt-stock-layout` | `INSTALL-OPENWRT-STOCK-LAYOUT` | 已校验非 UBI sysupgrade，布局为 `STOCK` 或 `OPENWRT_STOCK_LAYOUT` | 安排原厂布局安装。 |
| 更新 OpenWrt | `POST /api/install-ubi` | `INSTALL-UBI` + `X-Ursus-Keep-Settings: 1\|0` | 已校验 UBI sysupgrade，布局为 `OPENWRT_UBI`，无其他活动操作 | 开始 UBI 更新；答复 `reboot: MANUAL`。 |
| OpenWrt UBI 迁移 | `POST /api/install-ubi` | `INSTALL-UBI` + `X-Ursus-Keep-Settings: 0` | `STOCK`/`OPENWRT_STOCK_LAYOUT`，已校验 UBI preloader 及有效的 128 KiB BL2 候选 | 转换为规范 UBI 布局；BL2 最后写入。 |
| 重置 OpenWrt 设置 | `POST /api/reset-openwrt-settings` | `RESET-OPENWRT-SETTINGS` | OpenWrt 布局，无活动操作 | 只重建 `rootfs_data`。 |
| 检查 initramfs | `POST /api/initramfs-begin` + `…/initramfs-chunk` | — | — | 暂存并校验独立 FIT；只读。 |
| 单次启动 | `POST /api/expert/boot-once` | — | 已验证可启动 FIT | 从 RAM 启动，不写闪存；`automatic_sysupgrade: false`。 |
| 校验 UrsusBoot FIP | `POST /api/ursus-fip-begin` + `…/ursus-fip-chunk` | — | — | 暂存并校验 FIP；只读。 |
| 更新 UrsusBoot | `POST /api/update-ursusboot` | `UPDATE-URSUSBOOT` | 完整接收且验证的 FIP，无迁移或 FIP 更新活动 | 写 FIP 并读回校验。 |
| 重启进入 OpenWrt | `POST /api/reboot` | `REBOOT` | **此前闪存操作已完成** | 重启；否则返回 409。 |
| 运行（控制台） | `POST /api/console` | `X-Ursus-Command` | — | 执行真实 U-Boot 命令并返回输出；无白名单。 |

### 接收、校验与刷写流程

```text
选择文件 → begin 声明大小、会话代数和文件名
        → chunk 分块传入 RAM
        → 检查容器、FIT、哈希、fwtool 元数据、设备与布局
        → 状态/日志显示 OK 或明确的拒绝原因
        → 操作者确认，HTTP 端点登记操作
        → 主循环写入并读回，报告阶段与进度
        → 完成后手动重启
```

可识别 OpenWrt 非 UBI sysupgrade（tar）、带 fwtool 元数据的 UBI sysupgrade（FIT）、独立 FIT（仅 Expert），以及原厂 kernel/rootfs（可识别，但主固件通道不接受）。拒绝类别包括 `OK`、`UPLOAD_INCOMPLETE`、`BAD_CONTAINER`、`BAD_FIT`、`HASH_MISMATCH`、`METADATA_MISSING`、`DEVICE_MISMATCH`、`UNSUPPORTED_IMAGE_CLASS`、`IMAGE_TOO_LARGE`、`LAYOUT_UNSUPPORTED`、`OPERATION_LOCKED`。

### HTTP API

```text
GET  /                               网页
GET  /logo.svg  /favicon.svg         内嵌资源
GET  /api/status                     机器可读状态
GET  /api/log                        校验日志
GET  /api/operation-log              闪存操作日志
POST /api/console                    真实 U-Boot 命令
POST /api/firmware-begin|-chunk      主固件通道
POST /api/initramfs-begin|-chunk     Expert FIT 通道
POST /api/ubi-preloader-begin|-chunk 迁移 preloader
POST /api/ursus-fip-begin|-chunk     引导程序 FIP
POST /api/discard                    移除固件暂存
POST /api/expert/discard             移除 initramfs 暂存
POST /api/ubi-preloader-discard      移除 preloader 暂存
POST /api/ursus-fip-discard          移除 FIP 暂存
POST /api/install-openwrt-stock-layout   原厂布局安装
POST /api/install-ubi                    UBI 更新或迁移
POST /api/reset-openwrt-settings         重置 rootfs_data
POST /api/update-ursusboot               更新引导程序
POST /api/expert/boot-once               RAM 中启动 FIT
POST /api/reboot                         完成操作后重启
```

`begin` 使用 `X-Ursus-Total`、`X-Ursus-Generation`、`X-Ursus-Filename`；每个 `chunk` 使用 `X-Ursus-Offset`、`X-Ursus-Total`、`X-Ursus-Generation`。会话代数不符就废弃该次接收，防止混合不同上传。

## UrsusFlasher：推荐的电脑端编排器

[UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) 是 Windows/Linux 安装、备份与恢复工具。它识别 Airoha/Nokia 型号及当前启动/存储状态，创建并验证备份，从 Nokia 原厂系统、OpenWrt 或 WebFailsafe 安装/更新 UrsusBoot，选择相应传输通道，完成读回和救援流程；变砖时可使用 BootROM/XMODEM/UART。它也能传输本地新构建的 FIP。**UrsusBoot 是路由器中的引导/恢复框架，UrsusFlasher 是电脑上的操作层。**

## 基线与范围

最初公开的独立基线来自 Nokia XG-040G-MD 的 TEST61 谱系，后续扩展 MD/MF 板卡配置。历史 TEST57–TEST60 迭代脚本不属于公开构建入口。移植模型、致谢和完整开发背景目前请阅读[英文 README](README.md#porting-model)及 [PORTING.md](docs/PORTING.md)，此处暂不翻译。

## 安全与验证

软件构建或 CI 成功**不等于**实机验证。新板卡必须在有明确恢复路线的前提下完成目标硬件测试。UrsusBoot 可以修复启动链和重装 OpenWrt，却无法凭空恢复遗失的本机 MAC、序列号、GPON 与光学校准数据；操作前保存属于本机的可靠备份。

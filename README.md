# UrsusBoot

**Compact OpenWrt-aware recovery and installation U-Boot for Airoha router platforms.**

UrsusBoot grew out of the OpenWrt U-Boot bootloader ecosystem and targets Airoha devices where a small persistent recovery environment is especially valuable. It combines WebFailsafe, OpenWrt image awareness, UBI migration and recovery primitives while staying small enough for constrained boot areas.

## Why UrsusBoot

The main advantage is simple: **it provides much of the functionality normally associated with a much larger recovery environment while fitting into an approximately 512 KiB boot-area contract.** No Linux kernel or userspace is required just to validate, migrate and install OpenWrt images.

Typical capabilities include:

- WebFailsafe recovery UI;
- OpenWrt `sysupgrade` / FIT awareness;
- stock / stock-layout / canonical UBI detection;
- UBI creation and migration;
- FIP and bootloader update paths;
- readback verification and fail-closed validation;
- recovery over network and Airoha BootROM/UART paths.

## What UrsusBoot actually is

UrsusBoot is **not a rewrite and not a custom bootloader**. It is ordinary U-Boot — upstream U-Boot 2026.07 with the OpenWrt/Airoha platform work used by this tree — configured down to the facilities useful on the target routers, with the Ursus recovery layer added on top.

The TEST61 full configuration keeps 65 stock U-Boot commands, including the `mtd`/`ubi` stack, environment commands, networking (`ping`, `tftpboot`, `wget`, `dhcp`, `dns`, `sntp`, `mii`, `mdio`), booting, partition tools, hashes/CRC, compression support and the relevant GPIO/pinmux/LED controls. UrsusBoot adds seven commands of its own, for 72 commands in the resulting unlocked console.

The size reduction comes mostly from **not compiling hardware and subsystems that these NAND routers do not use**. The TEST61 configuration disables 95 command groups/subsystems, including filesystems such as UBIFS/ext/FAT/btrfs, unused storage buses such as USB/MMC/SATA/SCSI/NVMe/PCI/SPI-flash, EFI/video/bootmenu/bootstd, TPM, I2C and other unrelated facilities. `FIT_SIGNATURE`/RSA verification is disabled; FIT **hashes are verified**, FIT signatures are not.

What UrsusBoot adds on top of stock U-Boot includes:

- a WebFailsafe UI with a real U-Boot console in the browser;
- OpenWrt-aware staging, classification and fail-closed image validation;
- UBI creation, update and migration with readback verification;
- a boot dispatcher that chooses a usable path from what is actually on NAND and falls back to recovery;
- FIP/bootloader self-update and Airoha BootROM/UART recovery paths.

## Repository policy: self-contained

This repository is intended to be **self-contained**. A normal build must not clone or download code, templates or binary donors from any other projects.

The only external build dependency permitted by policy is an **official OpenWrt SDK/toolchain** appropriate for the target Airoha SoC. Board templates, boot-area templates, FIP lineage inputs, source, configuration and build scripts belong in this repository.

The Nokia XG-040G-MD and XG-040G-MF board templates included here are 512 KiB stock boot-area templates. Device identity is not sourced from these templates; model-specific identity such as MAC/serial/GPON data belongs to the RI/BOSA/device-identity path and must be handled separately.

## Build

Board, config, boot-area template, reference FIP lineage and allowed runtime roles are resolved from `config/board-profiles.json`; `build.sh` does not carry a board-name `case` table.

```bash
OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md persistent
OPENWRT_SDK=/path/to/openwrt-sdk ./build.sh xg040-md ram-recovery
```

The runtime role is real build input:

| Role | `bootcmd` | Purpose |
|---|---|---|
| `persistent` | `ursusdispatch` | Normal persistent supervisor boot from flash. |
| `ram-recovery` | `ursusweb;true` | RAM/recovery build that enters WebFailsafe directly. |

`ram-recovery` is applied at source level, so switching roles in one working tree requires restoring the mutated defaults first:

```bash
git checkout -- src/u-boot/defenvs src/u-boot/include
```

Profiles may also be explicitly described but not yet buildable. For example, a profile with `"config": null` fails with a direct `declares no 'config' yet` diagnostic instead of falling through to an unrelated build error.

### Persistent FIP packaging

For a profile with `reference_fip`, the reference file is **lineage/template input only**. It is not copied unchanged into the final image.

The build pipeline is:

```text
current source tree
      |
      v
 current u-boot.bin
      |
      v
 LZMA1EXT / 1 MiB dictionary / no EOPM
      |
      v
 current u-boot.lzma
      |
      +-----------------------------+
                                    |
reference FIP lineage --------------+--> repack NT_FW/BL33 + checksum
                                           |
                                           v
                                 dist/<board>/ursusboot-update.fip
                                           |
stock 512 KiB boot-area template ----------+
                                           |
                                           v
                              ursusboot-install-mtd0.bin
```

The builder verifies that the final `NT_FW/BL33` entry is byte-for-byte identical to the `u-boot.lzma` generated from the **current build**. This prevents a successful compile from accidentally emitting an install image that still contains the old reference TEST61 BL33.

`URSUS_FIP_TEMPLATE=/path/to/reference.fip` can override the profile lineage input; `URSUS_FIP` is retained as a compatibility alias for the same template role.

Outputs for XG-040G-MD are placed in `dist/xg040-md/`:

```text
u-boot.bin
u-boot.lzma
ursusboot-update.fip
ursusboot-install-mtd0.bin
SHA256SUMS
```

A board with no `reference_fip` still builds raw `u-boot.bin`, but does not pretend that a flashable FIP lineage exists.

## No donor dump required

For supported boards the build/install flow does not require a user to dump `mtd0` first.

```text
board stock template + freshly repacked UrsusBoot FIP
                         |
                         v
              ready 512 KiB install mtd0
```

For a completely bricked unit the same board template can be used by the Airoha BootROM/UART recovery flow. For a running Nokia stock firmware, a host installer can write the board-correct image through the rooted stock environment and verify readback. Device MAC/identity can be restored or configured separately from the sticker/device identity source when required.

## Repository layout

```text
build.sh                  single build entry point
scripts/                  host-side build and QA helpers
config/                   configs, Kconfig fragments and board profiles
boards/                   board policy headers + 512 KiB stock boot templates
reference/                proven FIP lineage inputs
src/u-boot/               complete U-Boot 2026.07 source tree + UrsusBoot
dist/                     generated artifacts
```

Important UrsusBoot additions inside `src/u-boot/` include `cmd/ursusdispatch.c`, `cmd/ursusweb.c`, `cmd/ursusubi.c`, `cmd/ursusupdate.c`, `cmd/ursusstock.c`, `cmd/ursusled.c`, the embedded WebFailsafe UI/logo includes and board-specific default environments/policies.

The standalone tree also carries the proven packaging helpers `lzma1ext_noeopm.c` and `repack_persistent_fip.py`, so FIP generation does not depend on another repository.

## UrsusFlasher: the recommended host orchestrator

[UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) is the companion **Windows and Linux installer/orchestrator/backup tool**. UrsusBoot is deliberately usable on its own, but UrsusFlasher provides the convenient operator layer around it:

- detects known Airoha/Nokia models and current boot/storage state;
- performs backups and validates them;
- installs or updates UrsusBoot from Nokia stock Linux, OpenWrt or UrsusBoot Recovery;
- performs readback verification and recovery workflows;
- handles Airoha BootROM/XMODEM/UART recovery for bricked devices;
- can transport a custom locally built UrsusBoot through the explicit Airoha UART/Brick-mode recovery path when the board-specific boot contract is correct.

In other words, **UrsusBoot is the bootloader/recovery framework; UrsusFlasher is the operator-friendly installer, backup and recovery workstation around it.**

## Baseline

The initial public standalone baseline is the hardware-proven TEST61 lineage used by the Nokia XG-040G-MD work, with subsequent modularization for XG-040G-MF / AN7583 and other Airoha targets. Historical TEST57-TEST60 iteration scripts are intentionally not part of the public build surface.

## Porting model

Porting should be board-policy driven rather than fork driven:

```text
UrsusBoot core
  +-- common WebFailsafe / image / UBI logic
  +-- Airoha SoC support
  +-- board policy
  |     +-- NAND geometry
  |     +-- boot/FIP layout
  |     +-- Ethernet/reset quirks
  |     +-- identity locations
  |     +-- supported image classes
  +-- storage policy
```

See `docs/PORTING.md`.

## Credits and lineage

UrsusBoot exists because a substantial amount of bootloader and OpenWrt work came before it. Particular thanks and respect go to:

- **Daniel Golle (dangowrt)** — OpenWrt U-Boot/UBI installation and recovery-oriented bootloader work: https://github.com/dangowrt
- **hanwckf** — MediaTek `bl-mt798x`, WebFailsafe, multi-layout and direct firmware installation work: https://github.com/hanwckf/bl-mt798x
- **Yuzhii0718** — continued MediaTek/Airoha bootloader and recovery work: https://github.com/Yuzhii0718
- **OpenWrt developers** — platform integration, image formats and the SDK/toolchain: https://github.com/openwrt/openwrt
- **U-Boot and Trusted Firmware-A developers** — the upstream boot stack on which this work rests.

UrsusBoot is a compact Airoha-focused continuation of ideas developed across that ecosystem, with board-policy, recovery, validation and transactional-safety work layered on top.

## Safety

A successful software build or CI run is **not** hardware validation. New board ports and new networking/FIP packaging changes remain unproven until tested on real hardware with a documented recovery path.

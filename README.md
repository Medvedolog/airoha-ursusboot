# UrsusBoot

**Compact OpenWrt-aware recovery and installation U-Boot for Airoha router platforms.**

UrsusBoot grew out of the OpenWrt/MediaTek bootloader ecosystem, but targets Airoha devices where a small persistent recovery environment is especially valuable. It combines WebFailsafe, OpenWrt image awareness, UBI migration and recovery primitives while staying small enough for constrained boot areas.

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

## Repository policy: self-contained

This repository is intended to be **self-contained**. A normal build must not clone or download code, templates or binary donors from UrsusFlasher, MedveFlasher, hanwckf, Yuzhii0718 or any other project.

The only external build dependency permitted by policy is an **OpenWrt SDK/toolchain** appropriate for the target Airoha SoC. Board templates, boot-area templates, FIP lineage inputs, patches, configuration and build scripts belong in this repository.

The Nokia XG-040G-MD and XG-040G-MF board templates included here are 512 KiB stock boot-area templates. Device identity is not sourced from these templates; model-specific identity such as MAC/serial/GPON data belongs to the RI/BOSA/device-identity path and must be handled separately.

## No donor dump required

For supported boards the build/install flow does not require a user to dump `mtd0` first.

```text
board stock template + UrsusBoot FIP
                 |
                 v
      ready 512 KiB install mtd0
```

Example:

```bash
URSUS_FIP=reference/md/ursusboot-test61-update.fip \
  ./build.sh xg040-md persistent
```

This emits `dist/xg040-md/ursusboot-install-mtd0.bin`.

For a completely bricked unit the same board template can be used by the Airoha BootROM/UART recovery flow. For a running Nokia stock firmware, a host installer can write the board-correct image through the rooted stock environment and verify readback. Device MAC/identity can be restored or configured separately from the sticker/device identity source when required.

## UrsusFlasher: the recommended host orchestrator

[UrsusFlasher](https://github.com/Medvedolog/airoha-router-ursusflasher) is the companion **Windows and Linux installer/orchestrator/backup tool**. UrsusBoot is deliberately usable on its own, but UrsusFlasher provides the convenient operator layer around it:

- detects known Airoha/Nokia models and current boot/storage state;
- performs backups and validates them;
- installs or updates UrsusBoot from Nokia stock Linux, OpenWrt or UrsusBoot Recovery;
- performs readback verification and recovery workflows;
- handles Airoha BootROM/XMODEM/UART recovery for bricked devices;
- can be used as a transport/orchestrator for a **custom locally built UrsusBoot**, including Airoha hardware not yet represented by a first-class UrsusFlasher board profile, through the explicit Airoha UART/Brick-mode recovery path.

In other words, **UrsusBoot is the bootloader/recovery framework; UrsusFlasher is the operator-friendly installer, backup and recovery workstation around it.**

## Baseline

The initial public standalone baseline is the TEST61 lineage used by the Nokia XG-040G-MD work, with subsequent board modularization for XG-040G-MF / AN7583 and other Airoha targets. Historical TEST57-TEST60 iteration scripts are intentionally not part of the public build surface; TEST61 is treated as the clean baseline.

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

- **Daniel Golle (dangowrt)** — for the OpenWrt U-Boot/UBI installation model, recovery-oriented bootloader work and the broader idea that the bootloader can understand OpenWrt rather than merely chainload it: https://github.com/dangowrt
- **hanwckf** — for the MediaTek `bl-mt798x` work, failsafe Web UI, multi-layout and direct firmware installation ideas that became a reference point for the wider router community: https://github.com/hanwckf/bl-mt798x
- **Yuzhii0718** — for continuing and extending that ecosystem, including modern WebFailsafe, DHCP/recovery and Airoha/MediaTek bootloader work used as a practical reference: https://github.com/Yuzhii0718
- **OpenWrt developers** — for the platform, image formats, U-Boot integration and the SDK/toolchain this project builds against: https://github.com/openwrt/openwrt
- **U-Boot and Trusted Firmware-A developers** — for the upstream boot stack on which all of this ultimately rests.

UrsusBoot is not presented as an isolated invention. It is a compact Airoha-focused continuation of ideas developed across that community, with its own board-policy, recovery, validation and transactional safety work layered on top.

## Safety

A successful software build or CI run is **not** hardware validation. New board ports must be treated as unproven until tested on real hardware with a documented recovery path.

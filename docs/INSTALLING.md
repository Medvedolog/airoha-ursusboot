# Installing UrsusBoot

This document is the operator-oriented installation and recovery guide. For most users the preferred path is **UrsusFlasher**, which handles model detection, transport selection, backup/readback checks and the supported recovery workflows around UrsusBoot.

## Which artifact do I use?

A successful persistent build can emit:

- `u-boot.bin` — raw current-source U-Boot payload; not normally flashed directly.
- `u-boot.lzma` — Airoha LZMA1EXT/no-EOPM BL33 payload produced from that `u-boot.bin`.
- `ursusboot-update.fip` — flashable FIP in which the donor container's NT_FW/BL33 entry has been replaced by the current build.
- `ursusboot-install-mtd0.bin` — complete 512 KiB boot-area image assembled from the board template plus the newly repacked FIP.
- `SHA256SUMS` — hashes for the artifacts produced by that build.

Do not confuse the FIP **donor** with the FIP **output**. `URSUS_FIP_DONOR`, `URSUS_FIP_TEMPLATE` and the legacy `URSUS_FIP` variable all name a donor/reference container. The build always replaces its BL33 with the current `u-boot.lzma`.

## Recommended path: UrsusFlasher

Use UrsusFlasher whenever possible. It can orchestrate the supported paths from:

1. UrsusBoot WebFailsafe;
2. rooted Nokia stock Linux;
3. OpenWrt;
4. Airoha UART/BootROM recovery for a bricked unit.

For a freshly built persistent image, hand UrsusFlasher the generated `ursusboot-update.fip` or the board-correct install image according to the workflow it requests. Let it perform the preflight, transport, write and readback checks rather than reproducing low-level commands by hand.

Repository:

https://github.com/Medvedolog/airoha-router-ursusflasher

## Recovery network trust model

WebFailsafe is a **physical recovery interface**, not a general-purpose management plane.

- HTTP has no authentication and no TLS.
- The service listens on all IPv4 interfaces while WebFailsafe is active.
- `/api/console` exposes the real U-Boot command line.
- Flashing and migration endpoints are available once their confirmation/precondition checks pass.

Use a trusted, isolated direct Ethernet link between the recovery workstation and the router. Do not leave WebFailsafe connected to a shared LAN, uplink, Wi-Fi bridge or other untrusted L2 segment.

## Route A: rooted stock Linux

Before writing anything, confirm the boot MTD geometry:

```sh
cat /proc/mtd
cat /sys/class/mtd/mtd0/bad_blocks
```

For the supported Nokia 512 KiB boot-area flow, `mtd0` must be the bootloader partition with:

```text
size       00080000
erase size 00020000
bad_blocks 0
```

Back up the full boot area:

```sh
dd if=/dev/mtd0 bs=131072 count=4 of=/tmp/mtd0-backup.bin
sha256sum /tmp/mtd0-backup.bin
```

Copy that backup to the workstation and verify the same SHA256 there. A backup that exists only in router RAM is not a recovery backup.

Transfer `ursusboot-install-mtd0.bin` to the router and verify its hash before writing.

Use **one writer only**. Once a writer has started it may already have erased a block, so do not switch to another writer merely because output looks unusual.

Typical writer choices are:

```sh
mtd write /tmp/ursusboot-mtd0.bin bootloader
```

or:

```sh
flash_erase /dev/mtd0 0 0
nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin
```

or:

```sh
flash_eraseall /dev/mtd0
nandwrite -p /dev/mtd0 /tmp/ursusboot-mtd0.bin
```

or:

```sh
mtd_debug erase /dev/mtd0 0 0x80000
mtd_debug write /dev/mtd0 0 0x80000 /tmp/ursusboot-mtd0.bin
```

After the write:

```sh
sync
dd if=/dev/mtd0 bs=131072 count=4 2>/dev/null | sha256sum
```

The readback SHA256 must match the install image before rebooting. If it does not match, keep the running system alive and fix the write while recovery is still available.

Some OpenWrt kernels expose the boot MTD read-only. UrsusFlasher has dedicated support for this case, including the pinned `mtd-rw` route and the narrow `ursus-mtd-raw` writer.

## Route B: live U-Boot console over UART

Use a **3.3 V** UART adapter:

```text
TX / RX / GND
115200 baud
8N1
no flow control
```

At a live U-Boot prompt, first inspect the MTD names:

```text
mtd list
```

Then receive the complete 512 KiB boot image:

```text
loadx 0x8e000000
```

Send `ursusboot-install-mtd0.bin` with XMODEM, then write the actual boot partition reported by `mtd list`. On a factory layout it is commonly `bl2`; on a canonical UrsusBoot/OpenWrt UBI layout it may be `ursus-ubi-bl2`.

Example only:

```text
mtd erase bl2 0x0 0x80000
mtd write bl2 0x8e000000 0x0 0x80000
```

Do not guess the partition name.

## Route C: fully bricked unit / Airoha BootROM

For a unit with no usable prompt, the proven recovery process is two-stage:

1. power off;
2. hold Reset before applying power;
3. send the proven Airoha preloader/BL2 over XMODEM;
4. send the proven RAM-installer FIP over XMODEM;
5. continue from the RAM-resident U-Boot;
6. install the persistent UrsusBoot build through the normal verified path.

The NAND is not modified merely by loading the BootROM stages into RAM.

The `ram-recovery` role in this repository sets `bootcmd=ursusweb;true`, but that alone does **not** prove that an arbitrary repacked FIP satisfies the hardware-proven BootROM stage-2 container/entry contract. Until that contract is validated on hardware, use UrsusFlasher's pinned RAM-installer for brick recovery.

## Recovery MAC behavior

UrsusBoot refreshes the factory MAC from the `ri` volume during `preboot` on every boot. `reset_factory` also re-derives and saves it after resetting the environment.

U-Boot probes Ethernet before `preboot`. Therefore, on an empty environment, you may briefly see:

```text
Warning: ... using random MAC address - ...
```

That warning means Ethernet was probed before the factory MAC was restored. The decisive line is the later:

```text
URSUS_MAC_SOURCE=RI ethaddr=...
```

Random MAC fallback is deliberately retained so a unit with damaged `ri` can still expose network recovery.

## What counts as success?

Software success and hardware success are different:

- **QA PASS**: repository structural/self-tests passed.
- **build PASS**: target U-Boot and packaging pipeline compiled successfully.
- **HW PASS**: the exact artifact was exercised on the target hardware with the documented recovery path and acceptance checks.

Do not promote CI/build evidence to HW PASS.

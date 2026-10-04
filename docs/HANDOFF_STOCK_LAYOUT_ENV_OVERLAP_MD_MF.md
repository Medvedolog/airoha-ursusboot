# HANDOFF / TODO — disable stock-layout OpenWrt as a target on MD/MF

Date: 2026-10-05  
Repo: `Medvedolog/airoha-ursusboot`  
Branch: `dev/ursusboot-http-backup`  
Baseline: `63e4e12939e9a3d2cb1137ae21ad4c747b8a57a1` (T79 source/QA baseline)

## Problem

Persistent UrsusBoot is intentionally kept.

The compatibility hazard is the combination:

```text
persistent UrsusBoot in original 512 KiB boot area
+
OpenWrt running in Nokia stock/factory NAND layout
+
later normal sysupgrade from that OpenWrt
```

For both XG-040G-MD and XG-040G-MF, upstream OpenWrt stock layout exposes:

```text
0x00000-0x7ffff  bootloader
0x60000-0x7ffff  u-boot-env   (one 128 KiB NAND eraseblock)
```

The logical env is at physical `0x7c000`, but `fw_setenv` rewrites the containing eraseblock `0x60000-0x7ffff`.

Both upstream MD and MF stock-layout `platform_pre_upgrade()` call `fw_setenv bootcmd ...` during sysupgrade. Current persistent UrsusBoot FIP can have live data in that eraseblock. A subsequent stock-layout sysupgrade can therefore destroy the FIP tail and produce on next boot:

```text
LZMA res 1 -> PANIC
```

The field MF incident is consistent with exactly this failure mode: restoring the saved `0x60000-0x7ffff` block restored byte equality.

## Important distinction

Current WebFailsafe stock-layout install itself is not the destructive event.

`src/u-boot/cmd/ursusweb.c::ursus_factory_install()` preserves the bootloader and writes only factory-layout kernel/rootfs areas. Therefore:

```text
persistent UrsusBoot
-> WebFailsafe
-> install non-UBI OpenWrt
-> boot it
```

can work.

The dangerous step is the later sysupgrade inside that running stock-layout OpenWrt.

## Decision

Do NOT:

- shrink the persistent FIP limit to `0x60000`;
- remove persistent UrsusBoot;
- redesign ONE-KEY around RAM-only UrsusBoot;
- use `/etc/fw_env.config` removal as the architectural fix.

Instead, stop creating **new stock-layout OpenWrt installations** on Nokia MD/MF.

Existing stock-layout installations remain detectable and migratable to UBI.

## P0 — UrsusBoot code change

Primary file:

`src/u-boot/cmd/ursusweb.c`

For Nokia XG-040G-MD and XG-040G-MF:

1. Do not offer/arm `POST /api/install-openwrt-stock-layout`.
2. Do not advertise `URSUS_STOCK_LAYOUT_INSTALL_ENABLED=1`.
3. A validated `URSUS_IMG_NONUBI_SYSUPGRADE` may still be classified for diagnostics, but must not become an install/update action.
4. Keep `OPENWRT_STOCK_LAYOUT` detection.
5. Keep migration from both `STOCK` and existing `OPENWRT_STOCK_LAYOUT` to `OPENWRT_UBI`.
6. Keep UBI->UBI update unchanged.
7. Keep stock restore/emergency paths unchanged.
8. Keep persistent UrsusBoot unchanged.

Desired policy:

```text
STOCK
  -> UBI migration only

OPENWRT_STOCK_LAYOUT
  -> UBI migration only

OPENWRT_UBI
  -> UBI update

NON-UBI / stock-layout OpenWrt
  -> classify if useful, but never install as a normal target
```

## P0 — QA

Add source-contract guards proving:

- `INSTALL_OPENWRT_STOCK_LAYOUT` cannot be armed for MD/MF;
- Web UI/API no longer advertises stock-layout install;
- non-UBI image classification does not imply write availability;
- STOCK -> UBI still works;
- existing OPENWRT_STOCK_LAYOUT -> UBI still works;
- UBI -> UBI still works;
- persistent FIP update/repair remains unchanged.

## P0 — rebuild

After source fix and QA:

1. Build both MD and MF persistent artifacts from the same new UrsusBoot revision.
2. Record exact commit SHA and artifact SHA256/size.
3. Status after CI/build is **BUILD_VERIFIED / CI_VERIFIED only**.
4. Hardware status remains **HW_PENDING** until MD and MF are exercised.

Do not reuse T78/T79 artifact names for the changed binary. Use the next dev/test version according to project convention.

## P1 — UrsusFlasher integration

Repo: `Medvedolog/airoha-router-ursusflasher`

After the new UrsusBoot build:

- update the pinned UrsusBoot commit/artifacts;
- align `MANIFEST.json` / capabilities / EXPERT policy so stock-layout OpenWrt is no longer a normal target;
- keep `OPENWRT_STOCK_LAYOUT` as a recognized migration/recovery source;
- preserve normal ONE-KEY:
  `Nokia factory -> persistent UrsusBoot -> Recovery -> STOCK->UBI`.

Related UrsusFlasher handoff:

`docs/HANDOFF_STOCK_LAYOUT_ENV_OVERLAP_MD_MF.md`

## Current status

T79 baseline `63e4e12939e9a3d2cb1137ae21ad4c747b8a57a1` does **not** yet contain this fix.

No source code was changed by this handoff. `main` must remain untouched unless explicitly requested.

# Porting UrsusBoot

This document describes the **current** porting contract, including what is implemented and what is still scaffolding.

## First rule: do not infer destructive geometry

Before adding a board, establish from real evidence:

- SoC and exact compatible string;
- NAND geometry and erase size;
- stock boot-area boundaries;
- FIP physical offset and maximum usable size;
- environment ownership/location;
- stock kernel/master/slave offsets if applicable;
- UBI layout expectations;
- identity storage locations;
- reset/UART/BootROM recovery procedure.

Do not copy another board's offsets merely because the SoC matches.

## Current build model

The production build currently consumes a **complete config** from the profile's `config` field.

The registry also contains:

- `fragments`;
- `board_policy_header`;
- `boot_policy`;
- `layout_policy`;
- `environment_policy`.

Those fields describe the intended modular architecture, but the fragment/policy helpers are **porting scaffolding today**, not the authoritative production build path. `build.sh` does not yet synthesize a new board config from the fragment list.

This distinction is deliberate: MD/MF use proven full configs rather than silently composing an unproven target.

## Profile schema

Add a profile to `config/board-profiles.json`.

Example shape:

```json
{
  "vendor": "Vendor",
  "model": "Model",
  "compatible": "vendor,model",
  "soc": "an7581",
  "fragments": [
    "ursusboot-common.cfg",
    "ursusboot-soc-an7581.cfg",
    "ursusboot-board-example.cfg",
    "ursusboot-storage-ubi-redundant.cfg"
  ],
  "board_policy_header": "example.h",
  "boot_policy": "example-stock-openwrt",
  "layout_policy": "example-ubi",
  "environment_policy": "ubi-redundant",
  "derivation": "native",
  "default_role": "persistent",
  "allowed_roles": ["persistent", "ram-recovery"],
  "config": "example.full.config",
  "boot_area_template": "boards/example/stock-mtd0-template.bin",
  "reference_fip": "reference/example/proven-donor.fip"
}
```

Use `null` rather than inventing a path for a component that is not yet proven. The resolver and QA intentionally reject an attempted complete build when required fields are absent.

## Board policy header

A board policy header belongs under `boards/` and records board-specific constants/constraints such as:

- stock offsets and slot sizes;
- UBI/FIT expectations;
- stock header family;
- environment ownership;
- SerDes/PHY quirks;
- writable spans;
- model-specific limits.

Keep SoC-generic behavior out of a board header.

## Full config

Until fragment synthesis becomes an enforced build step, a new buildable board needs a reviewed full config under `config/`.

Minimum areas to review explicitly:

- target SoC/board;
- default device tree;
- NAND/SPI-NAND/UBI support;
- Ethernet/MDIO/PCS/PHY;
- lwIP commands required by recovery;
- default environment source;
- environment backend/overwrite semantics;
- WebFailsafe dependencies;
- `CONFIG_USE_PREBOOT` if MAC refresh relies on it;
- `CONFIG_ENV_OVERWRITE=y` for boards whose recovery logic must correct `ethaddr`;
- `CONFIG_NET_RANDOM_ETHADDR=y` if a damaged identity store must still retain network recovery.

Do not remove random-MAC fallback merely to make logs cleaner; on a damaged `ri` that can turn a recoverable unit into one with no network path.

## Boot-area template

The committed template must be generic for the board and must not contain device identity/credentials.

Never commit:

- serial numbers;
- GPON credentials;
- device-specific RI/BOSA dumps;
- customer MACs;
- backups from a live unit.

Document the template's size, erase geometry, preserved regions and provenance.

## Donor FIP

A donor FIP is a **container lineage input**, not the final bootloader.

The persistent build must:

1. build current `u-boot.bin`;
2. pack it into current `u-boot.lzma`;
3. replace donor NT_FW/BL33 only;
4. verify the output contains exactly that fresh BL33;
5. enforce the container/boot-area size budget.

If no board-correct proven donor exists, leave `reference_fip: null`. Do not borrow another board's FIP merely to make the pipeline produce a file.

## Runtime role

A board may expose:

- `persistent` — normal supervisor/dispatcher;
- `ram-recovery` — direct WebFailsafe role.

A `ram-recovery` BL33 does not automatically prove that the surrounding FIP is valid for the Airoha BootROM stage-2 RAM-installer contract. Treat those as separate proofs.

## Required validation before calling a port usable

### Source/QA

- registry resolves;
- paths exist;
- scripts compile;
- offline QA passes;
- no foreign-repository runtime/build dependency appears.

### Build

- target compiler builds the exact profile;
- no unresolved prototype/link errors;
- current BL33 passes the FIP byte-identity check;
- generated FIP stays within its budget;
- install image is exactly the declared boot-area size.

### Hardware

At minimum:

- UART boot log captured;
- recovery web reachable;
- stable/expected MAC verified;
- ping and TFTP while WebFailsafe is alive;
- firmware validation/upload path exercised;
- bootloader update/readback exercised if applicable;
- Ctrl-C WebFailsafe stop/restart path checked;
- power-cycle recovery confirmed;
- documented brick-recovery path independently available.

**CI PASS != build PASS != HW PASS.**

## XG-140G-MD as an example of an intentionally incomplete port

The registry already describes `xg140-md` and has policy scaffolding, but `config`, `boot_area_template` and `reference_fip` are null. That is intentional. The profile documents what is known without pretending that a flashable build has been proven.

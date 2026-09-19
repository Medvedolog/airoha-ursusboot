# Provenance and binary lineage

## Standalone repository origin

The standalone import baseline came from:

```text
Medvedolog/airoha-router-ursusflasher
commit e7f96d507c3c54deab62c845aa8081fff052dc8c
```

The hardware-proven MD baseline is the UrsusBoot `0.1.0-alpha5-UBIUX1-TEST61` lineage.

The standalone repository intentionally does not depend on the UrsusFlasher repository at build time. Source, board templates, config, donor/reference inputs and packaging helpers required for a normal build live here.

## U-Boot/OpenWrt lineage

UrsusBoot is based on the OpenWrt/Airoha U-Boot 2026.07-era source line rather than being a clean-room bootloader.

The project adds a compact recovery/application layer on top of that base:

- WebFailsafe and browser console;
- OpenWrt image validation/classification;
- UBI update/migration;
- stock bridge boot;
- boot dispatcher;
- bootloader/FIP update path;
- board-specific recovery policy.

## Donor FIP lineage

The committed MD reference FIP is a **donor container**, not a final binary to copy unchanged into every build.

The build uses its proven platform/container lineage and replaces the NT_FW/BL33 payload with the LZMA1EXT/no-EOPM form of the U-Boot compiled from the current source tree.

The output is then checked so the embedded BL33 is byte-for-byte the current `u-boot.lzma`.

This distinction is essential when reproducing or auditing a build.

## Board templates and identity

Committed Nokia boot-area templates are generic board templates. Device identity is handled separately through RI/BOSA/device-specific storage.

Do not derive or commit per-device identity from a template, and never add live-device backups, serials, MACs or GPON credentials to the repository.

## Historical material intentionally omitted

TEST57-TEST60 iteration scripts and transition-debug archaeology are not part of the supported public build surface. TEST61 is treated as the clean standalone MD baseline, with later fixes and modularization tracked normally in Git history.

## Reproducibility boundary

Git identifies the source/tree state, but target output also depends on the exact external OpenWrt cross-toolchain. Release-quality provenance should therefore record:

- UrsusBoot commit SHA;
- board/profile and runtime role;
- OpenWrt SDK/toolchain archive identity;
- toolchain SHA256;
- produced artifact SHA256 values;
- whether evidence is QA, build or hardware validation.

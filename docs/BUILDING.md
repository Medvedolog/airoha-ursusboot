# Building UrsusBoot

## Requirements

A normal standalone build is self-contained except for the target toolchain.

Required host components:

- Python 3;
- GNU make and normal build utilities;
- host C compiler, default `gcc`;
- liblzma development headers/libraries;
- an extracted official OpenWrt AArch64 SDK or toolchain.

Set:

```sh
export OPENWRT_SDK=/path/to/openwrt-sdk-or-toolchain
```

The build searches that tree for an AArch64 OpenWrt compiler and derives `CROSS_COMPILE` automatically.

## Supported profile state

The live registry is `config/board-profiles.json`.

Current intent:

| Profile | SoC | Config | Boot template | Donor FIP | Build status |
|---|---|---|---|---|---|
| `xg040-md` | AN7581 | `u-boot.TEST61.full.config` + fragments | yes | `reference/md/` | persistent FIP + 512 KiB install image |
| `xg040-mf` | AN7583 | `an7583_nokia_xg-040g-mf_MF2_RAM_defconfig` + fragments + `ursusboot-runtime-mf.cfg` | yes | `reference/mf/` (MedveFlasher rc35) | persistent runtime (`u-boot.runtime.lzma`) + UART RAM-recovery pair |
| `xg140-md` | AN7581 | not declared | not declared | not declared | described/scaffolded, intentionally not buildable |

A profile with `config: null` is expected to fail explicitly rather than produce a guessed build.

## Build pipeline

`build.sh <board> [role]` never touches `src/u-boot`. It copies the tree to `work/<board>-<role>/u-boot` and applies, in order:

1. profile `source_transforms` (MF: `mf-runtime`, the HW-cycled MF derivation in `scripts/mf/`);
2. runtime role (`scripts/apply_runtime_role.py`);
3. `env_overlay` for the role (MF runtime environment);
4. board policy header `boards/<board_policy_header>`;
5. identity from `VERSION` (`.scmversion`, `URSUS_VERSION`);
6. optional UBI preloader pin (`URSUS_UBI_PRELOADER`, see below);
7. `config` + profile `fragments` + `role_fragments`, merged and re-checked after `olddefconfig`;
8. `config_require` / `config_forbid`, compile, `binary_require` / `binary_forbid`;
9. `packaging`: `persistent-fip` (MD) or `mf-runtime` (MF).

`xxd` is required by U-Boot's environment generator; if the host has none, `build.sh` uses the SDK's `scripts/xxdi.pl`.

## UBI preloader pin and fast-scan BL2

UrsusBoot's STOCK->UBI migration accepts a preloader only by compiled-in SHA256 digests (preloader and 128 KiB BL2 candidate). Without `URSUS_UBI_PRELOADER` the HW-proven MD/MF preloader digests stay compiled in. To build for another preloader (for example the fast-scan BL2):

```sh
python3 scripts/atf/wrap_bl2_preloader.py --bl2 an7581-bl2.bin --out preloader.fip
URSUS_UBI_PRELOADER=preloader.fip OPENWRT_SDK=... ./build.sh xg040-md
```

The release path does all of this from an official OpenWrt checkout at `config/fast-bl2.json` `openwrt_ref`:

```sh
OPENWRT_DIR=/path/to/openwrt ./scripts/ci/build-release.sh xg040-md
```

It builds the host tools and toolchain, applies `scripts/atf/atf-airoha-083c14f-ubi-scan-fastpath.patch` inside the ATF package's `Build/Prepare` (`scripts/atf/hook_atf_fastpath_patch.py`), proves the compiled tree is patched, wraps the BL2, builds UrsusBoot pinned to it with the same OpenWrt toolchain and writes `dist/<board>/PROVENANCE.json`. CI (`.github/workflows/build.yml`) runs it for MD and MF.

The fast scan exists only in BL2 (ATF). UrsusBoot's own `ubi part` still performs a full UBI scan (`CONFIG_MTD_UBI_FASTMAP` is off).

## Persistent build

For XG-040G-MD:

```sh
OPENWRT_SDK=/path/to/toolchain ./build.sh xg040-md persistent
```

The pipeline is:

```text
profile/config
   -> apply runtime role
   -> make olddefconfig
   -> build current u-boot.bin
   -> LZMA1EXT/no-EOPM pack
   -> donor FIP repack (replace NT_FW/BL33 only)
   -> verify embedded BL33 == current u-boot.lzma
   -> merge into board boot-area template
   -> dist/<board>/SHA256SUMS
```

Expected MD outputs:

```text
dist/xg040-md/u-boot.bin
dist/xg040-md/u-boot.lzma
dist/xg040-md/ursusboot-update.fip
dist/xg040-md/ursusboot-install-mtd0.bin
dist/xg040-md/SHA256SUMS
```

## Donor FIP override

The board profile normally selects the proven donor/reference FIP.

Canonical override:

```sh
URSUS_FIP_DONOR=/path/to/proven-donor.fip \
OPENWRT_SDK=/path/to/toolchain \
./build.sh xg040-md persistent
```

Compatibility aliases:

- `URSUS_FIP_TEMPLATE`
- `URSUS_FIP` (legacy name)

All three have **donor semantics**. None means "copy this FIP unchanged to the output".

The donor supplies the proven container/platform lineage; the current build supplies BL33.

## Runtime roles

`persistent`:

```text
bootcmd=ursusdispatch
```

`ram-recovery`:

```text
bootcmd=ursusweb;true
```

The role is currently applied at source level. A `ram-recovery` invocation modifies the default-environment source in the worktree. Restore before switching roles again:

```sh
git checkout -- src/u-boot/defenvs src/u-boot/include
```

## Host compiler override

The LZMA1EXT packer is built on the host. Override the compiler if needed:

```sh
HOSTCC=clang OPENWRT_SDK=/path/to/toolchain ./build.sh xg040-md
```

If the host compiler or liblzma development files are unavailable, the build fails. It must never silently substitute the donor's old BL33.

## QA

Run:

```sh
bash scripts/qa.sh
```

QA checks, among other things:

- board-template blob identity and size;
- profile registry paths and roles;
- donor FIP parse/repack selftest;
- self-contained-repository policy;
- shared lwIP netif invariants;
- deferred web-console invariants;
- MAC preboot and `CONFIG_ENV_OVERWRITE` guards;
- negative role/profile cases;
- absence of a stale repository-wide `SHA256SUMS`.

A QA PASS does not prove compilation and does not prove hardware behavior.

## Reproducibility notes

The repository keeps source/config/template/donor inputs locally. The external OpenWrt toolchain still matters: compiler version and SDK snapshot can change the output. For a release-quality build, record the exact toolchain archive and SHA256 alongside the artifact hashes.

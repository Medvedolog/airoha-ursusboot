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
| `xg040-md` | AN7581 | `u-boot.TEST61.full.config` | yes | yes | complete persistent packaging path |
| `xg040-mf` | AN7583 | `an7583_nokia_xg-040g-mf_MF2_RAM_defconfig` | yes | not yet declared | raw build path; no complete persistent FIP lineage yet |
| `xg140-md` | AN7581 | not declared | not declared | not declared | described/scaffolded, intentionally not buildable |

A profile with `config: null` is expected to fail explicitly rather than produce a guessed build.

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

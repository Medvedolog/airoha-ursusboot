# Changelog

This file records meaningful UrsusBoot milestones and, where relevant, the level of evidence behind them.

Evidence labels used here:

- **SOURCE** — source/configuration/documentation change exists in the repository.
- **QA PASS** — repository QA passed for the exact commit.
- **BUILD PASS** — the target build and packaging pipeline completed for the exact commit/run stated.
- **HW PASS** — the exact artifact/behavior was exercised successfully on real target hardware.

**QA PASS and BUILD PASS are not HW PASS.**

## 0.1.0-alpha5-t64 (TEST64, branch `test63`)

Version string shortened to `0.1.0-alpha5-t64` (was `0.1.0-alpha5-UBIUX1-TEST64`); `UBIUX1` no longer distinguished anything. The release build applies OpenWrt PR 24025 (Fudan FM25G02B) before building the Vanilla U-Boot.

TEST64 adds the last leg of the STOCK -> OpenWrt path: replacing UrsusBoot by the Vanilla OpenWrt U-Boot, as a separate, explicitly named one-way operation. Evidence: **SOURCE**, **QA PASS** (standalone QA), local MD/MF builds with stand-in Vanilla FIPs. **HW PENDING.**

### Vanilla replacement (firmware)

- Reuses the UrsusBoot self-update transaction unchanged: UBI `fip.new` -> readback -> atomic `fip`->`fip.old`, `fip.new`->`fip` -> verify -> rollback. UrsusBoot is kept as `fip.old`.
- A separate candidate kind with its own validator (`ursus_vanilla_fip_validate`): U-Boot 2026.07 **without** UrsusBoot lineage, this board's compatible and not the sibling board's (from `boards/*.h`: `URSUS_BOARD_COMPATIBLE`, new `URSUS_BOARD_OTHER_COMPATIBLE`), and the **SHA256 pinned at build time** (`ursus_vanilla_fip_sha256`, all zero = none pinned = refused).
- OpenWrt UBI layout only, and only if the installed BL2 (NAND 0..128 KiB) equals the pinned fast BL2 (`ursus_ubi_installed_bl2_matches_pin`).
- The UrsusBoot self-update keeps its validators (MD in source, MF via its derivation) and never accepts a Vanilla FIP; the Vanilla path never accepts UrsusBoot.
- Console: `ursusupdate vanilla-check <addr> <len>`, `ursusupdate vanilla-write <addr> <len> REPLACE-URSUSBOOT-WITH-VANILLA`.

### WebFailsafe

- Own upload kind sharing the FIP stage buffer: `/api/vanilla-fip-begin|chunk|discard`; own operation `/api/replace-with-vanilla` with `X-Ursus-Confirm: REPLACE-URSUSBOOT-WITH-VANILLA`. `/api/update-ursusboot` is unchanged.
- `/api/status`: `vanilla_fip_pinned`, `vanilla_fip_valid`, upload progress, `bootloader_update_kind`.
- UI: "Replace UrsusBoot with Vanilla U-Boot" in the UrsusBoot update pane, enabled only after the pinned FIP validates; warns that WebFailsafe is gone after reboot (recovery: USB-UART).

### Build

- `scripts/ci/build-release.sh` builds OpenWrt `uboot-airoha` for the board variant (`uboot_variant` in the profile) in the same tree as the fast BL2, and `scripts/vanilla/make_vanilla_fip.py` assembles the Vanilla FIP from the **same donor FIP as the UrsusBoot FIP** with only NT_FW (BL33) replaced (MD: `repack_persistent_fip.rebuild`, MF: two-entry repack). BL31 and all other entries equal the UrsusBoot chain.
- `build.sh` pins it (`URSUS_VANILLA_FIP`, `scripts/pin_vanilla_fip.py`), checks the digest bytes are compiled in, ships `vanilla-u-boot.fip` (+ `vanilla-u-boot.bin`), records `VANILLA_FIP_SHA256` in BUILD-INFO and `vanilla_fip_sha256` in PROVENANCE.

## 0.1.0-alpha5-UBIUX1-TEST63 (branch `test63`)

TEST63 makes this repository the single firmware source-of-truth for the Airoha UrsusBoot line and the modular build contract real. `airoha-router-ursusflasher` consumes TEST63 artifacts from an exact commit of this repository.

### Modular build pipeline

- `build.sh` builds in `work/<board>-<role>/u-boot`; `src/u-boot` is never mutated.
- Pipeline per profile: source transforms -> runtime role -> env overlay -> **board policy** (`boards/*.h`, previously scaffolding) -> identity from `VERSION` -> optional UBI preloader pin -> full config + **Kconfig fragments merged and checked** -> compile -> per-profile packaging.
- Per-profile `config_require` / `config_forbid` and `binary_require` / `binary_forbid` contracts, including Fudan FM25G01B/FM25G02B SPI-NAND support, board identity and the UBI preloader digests actually compiled in.
- `BUILD-INFO.txt` per build (commit, epoch, donor digest, preloader pin).

### XG-040G-MF becomes a real persistent target

- `xg040-mf` now builds the persistent MF runtime from the same `src/u-boot`: the HW-cycled UrsusFlasher MF derivation (`scripts/mf/`) is applied as a source transform, the board policy supplies HDR3/SerDes/identity.
- Outputs `u-boot.runtime.lzma`, `ursusboot-runtime-ram.fip` (UART/BootROM RAM recovery) and `ursusboot-uart-preloader.bin`.
- MF donor FIP, UART preloader and the MedveFlasher FIP parser/encoder are vendored with provenance (`reference/mf/`, `scripts/mf/medve/`); MedveFlasher is no longer a build dependency.
- The MF runtime environment gains the RI-derived MAC logic already used on MD (`preboot=run ethaddr_factory`, `reset_factory`).
- Only the `persistent` role is offered for MF in this pipeline.
- Fixed an MD-only assumption in the WebFailsafe layout probe: `cmd/ursusweb.c` matched the stock slot header only against `HDR2`, so on XG-040G-MF (`HDR3`) `/api/status` reported `stock_fit=false` / `STOCK_INCOMPLETE` for a healthy stock layout. The board policy now substitutes `URSUS_BOARD_STOCK_HDR_MAGIC` there as it already did in `cmd/ursusstock.c` (MD unchanged: `HDR2`). Found through the UrsusFlasher TEST62 MF CI binary contract.

### Fast-scan BL2 and preloader pinning

- UrsusBoot accepts a STOCK->UBI preloader only if its SHA256 and the SHA256 of the 128 KiB BL2 candidate (`0x800 x 0xff` + preloader + `0xff` padding) equal digests compiled into `cmd/ursusubi.c`. A new BL2 is therefore rejected (`URSUS_UBI_PRELOADER_REJECT reason=sha256`) unless UrsusBoot is built for it.
- `scripts/pin_ubi_preloader.py` + `URSUS_UBI_PRELOADER=<preloader.fip> ./build.sh ...` compile in the digests of exactly that preloader; the build fails if an old digest survives.
- `scripts/atf/`: the ATF UBI scan fast-path patch, a hook that applies it inside OpenWrt's `Build/Prepare` (patching `build_dir` after `prepare` was silently lost when `compile` re-extracted the sources), and a BL2 -> preloader FIP wrapper byte-identical to `fiptool create --tb-fw`.
- `scripts/ci/build-release.sh` + `.github/workflows/build.yml`: fast BL2 (OpenWrt at `config/fast-bl2.json`) -> preloader FIP -> pinned UrsusBoot -> packaging -> `PROVENANCE.json`, for MD and MF.

### Evidence

- Local cross-builds of `xg040-md` and `xg040-mf` (OpenWrt SDK r35906), with and without `URSUS_UBI_PRELOADER` -> **BUILD PASS** (local, not CI).
- `scripts/qa.sh` including the new `scripts/qa-pipeline.sh` -> **QA PASS** (local).
- CI BUILD PASS and **HW PASS: pending**. The MF fast-BL2 STOCK->UBI path and the MF RI-derived MAC path have never run on hardware.

## Unreleased

### Documentation and project structure

- Reworked the standalone documentation into operator-facing guides:
  - `docs/BUILDING.md`
  - `docs/INSTALLING.md`
  - `docs/TESTING.md`
  - `docs/PORTING.md`
  - expanded `docs/PROVENANCE.md` and `docs/CREDITS.md`.
- Documented the WebFailsafe trust boundary: recovery is intended for a trusted isolated direct Ethernet link, not a shared/untrusted LAN.
- Documented donor-FIP semantics and the distinction between donor/reference input and newly repacked output.
- Documented the current porting boundary: full configs are authoritative today; Kconfig fragments/policy headers remain scaffolding until the build pipeline consumes them directly.
- Removed the stale repository-wide `SHA256SUMS`; each build continues to emit `dist/<board>/SHA256SUMS`.

Evidence: **SOURCE**, exact QA on the documentation expansion commit `7dc9833948b0cdf56e1765d718500e557620d01e`, run `35422876971` — **QA PASS**.

### Recovery MAC identity

- Moved factory-MAC restoration to the `preboot` path so `ethaddr_factory` runs on every boot.
- `reset_factory` now resets the environment, re-derives the MAC from `ri`, and saves both environment copies.
- Added typed diagnostics such as `URSUS_MAC_SOURCE=RI` and explicit fallback warnings.
- Kept `CONFIG_NET_RANDOM_ETHADDR=y` deliberately as the last-resort network-recovery path for damaged/missing identity storage.
- Added `CONFIG_ENV_OVERWRITE=y` to the XG-040G-MF configuration so a previously saved or wrong `ethaddr` can be corrected rather than being blocked by write-once environment flags.
- QA now asserts both `CONFIG_USE_PREBOOT=y` and `CONFIG_ENV_OVERWRITE=y` for the current MD/MF profiles.

Relevant commits:

- `9956b684be7e0665d5d17c9ac937e190a089b24c` — stabilize recovery MAC and document trust boundary.
- `fc861676a4f9f97598eb25b7dbb2339b301de771` — MF `CONFIG_ENV_OVERWRITE=y`.
- `e5ae5747091238d2ebc78bdb6726413b264b674c` — clean QA guards for the final MF/MAC state.

Evidence:

- Exact MD cross-build run `35368632518` on `3077533a9897423ede688b12889850aca252374d` — **BUILD PASS**.
- Artifact `ursusboot-xg040-md-macfix-build`, artifact ID `10556634434`, ZIP digest `sha256:bde86d1a632494e71474e65be74754ac71bde398f097d4f93ab79da7c658ae26`.
- Exact final QA run for `e5ae5747091238d2ebc78bdb6726413b264b674c` — **QA PASS**.
- Real-device MAC-stability acceptance remains pending — **no HW PASS claimed**.

### Shared lwIP ownership

- WebFailsafe, `ping`, and TFTP now share the active lwIP netif instead of tearing each other down.
- Network commands fail closed if an incompatible service owns the active interface.
- Fixed the `ping_raw_init()` error path so a borrowed WebFailsafe netif is not removed on failure.
- UART Ctrl-C cleanly stops WebFailsafe and returns to the U-Boot prompt; `ursusweb` can then be started again.
- Web-console commands are deferred out of the TCP receive callback and the connection/PCB is revalidated after execution.

Relevant commits:

- `63f874ed1ab9648e1b6ef06a879a3426eed35c5d` — shared WebFailsafe/ping/TFTP netif ownership.
- `82ad0f7a2b4059de45b80e1ee2bc11a67ef2b052` — QA guards for shared-lwIP ownership.
- `3afbaab26ee9637c2bae9309b34d3f0e118aafb8` — preserve borrowed WebFailsafe netif on ping init failure.
- `3212cc6e705a56d7f8865fdab379d48ff7efe0aa` — QA guard for the ping error path.

Evidence:

- Exact QA run `35272286737` for `82ad0f7a2b4059de45b80e1ee2bc11a67ef2b052` — **QA PASS**.
- Later MD cross-builds compiled the shared-netif source successfully — **BUILD PASS** for those builds.
- Full multi-operation hardware acceptance remains a separate **HW PASS** requirement.

### Standalone build and packaging

- Standalone repository made self-contained: source, configs, board templates, donor/reference inputs, packaging helpers and QA live in this repository.
- Build is data-driven through `config/board-profiles.json`.
- Runtime roles:
  - `persistent` → `bootcmd=ursusdispatch`
  - `ram-recovery` → `bootcmd=ursusweb;true`
- Build now always packages the freshly compiled U-Boot:
  - current `u-boot.bin`
  - Airoha LZMA1EXT/no-EOPM `u-boot.lzma`
  - donor/reference FIP repack with NT_FW/BL33 replacement
  - byte-identity verification of the embedded BL33
  - complete 512 KiB install image.
- Canonical donor override is `URSUS_FIP_DONOR`; `URSUS_FIP_TEMPLATE` and legacy `URSUS_FIP` are compatibility aliases with the same donor semantics.
- Added a FIP repack self-test that reproduces the reference container byte-for-byte when repacking its own NT_FW payload.

Evidence:

- MD persistent cross-build on `0c2c5c2b40e302abaf4f9f854e3c653f4b7f6256`, run `35274863868` — **BUILD PASS**.
- Raw U-Boot size: 861472 bytes.
- LZMA BL33 size: 292080 bytes.
- BL33 margin to first certificate boundary: 35,600 bytes (34.77 KiB).
- Generated FIP SHA256: `fcd5c167cb7c824d24f7585f93e0e398178b2bfc7e50e0f038e67cb57ee6a94f`.
- Generated 512 KiB install image SHA256: `ef0d75539d85b20f2b872355360cfad16c2eba2f2ccd68b31ca481783bf2570b`.
- This proves compilation and packaging, not real-device behavior.

### Board/profile state

- `xg040-md` — AN7581, complete persistent packaging profile with board template and proven donor/reference FIP.
- `xg040-mf` — AN7583, board template and build config present; persistent donor-FIP lineage is not yet declared in the profile.
- `xg140-md` — described as an intentional incomplete/scaffolded profile; `config`, `boot_area_template` and `reference_fip` remain null so the build fails explicitly rather than guessing.

## Historical development lineage — original UrsusFlasher repository

Before the standalone repository existed, UrsusBoot was developed inside
`Medvedolog/airoha-router-ursusflasher`. The most useful historical sources are the
published changelogs, the TEST57-TEST61 hardware-test notes, per-build `BUILD_INFO`
files, and later handoff documents on the development branches.

This section records that lineage because it explains why several present-day
standalone invariants exist. It does **not** mean every historical UrsusFlasher host
feature or experimental branch is part of the standalone UrsusBoot product.

### 0.1.0-alpha3 — emergency/BootROM baseline

Alpha3 became the pinned emergency boundary for the MD / AN7581 path.

Known artifacts from the original repository:

- persistent FIP: `ursusboot-md-0.1.0-alpha3-update.fip`
  - SHA256 `597071e178470bfda23aab9738ad7ddb0b25e9b21ef336fd3eceb39c39f983ce`
- RAM installer FIP: `ursusboot-md-0.1.0-alpha3-ram-installer.fip`
  - SHA256 `dc08ed0be1b1d68f6bc247ae45293e6ab0ca9df7695541b664e4060a145228c8`
- BL2: `ursusboot-md-0.1.0-alpha3-bl2.bin`
  - SHA256 `6f9c928bad500de0339bbfdfa354c17a7ac044f96c913f3a01301971d6cd659d`

The old repository explicitly describes this alpha3 RAM/BootROM chain as the exact
hardware-working emergency lineage. Later production FIPs were deliberately kept
separate from it: normal self-update must not silently replace the proven emergency
preloader/RAM-installer contract.

Evidence: **HW PASS** for the historical emergency lineage as documented by the
original UrsusFlasher payload/EMERGENCY contract. This does not automatically confer
BootROM-stage-2 HW proof on a newly repacked standalone `ram-recovery` FIP.

### 0.1.0-alpha4 — recovery hardening series

The alpha4 line was not one monolithic change; it accumulated several focused
hardware/recovery iterations.

#### HWFIX1

- disabled the production boot menu and retained `bootcmd=ursusdispatch`;
- retained the sticky 750 ms Reset-at-boot recovery trigger;
- enabled console recording used by the browser console path;
- added explicit re-entry/ownership behavior for WebFailsafe and lwIP;
- included the staged UBI-update/status work and LED diagnostics.

Historical artifact:

- FIP SHA256 `f07245f705227e981260984a63572f9ddee7bc0a8f482c353e2e278ff2bfb04f`.

The subsequent HWFIX2 build contract names HWFIX1 as its hardware-proven persistent
FIP base.

#### HWFIX2

- completed the recovery LED/error indication before NAND/UBI diagnostics;
- avoided a duplicate full raw-MTD bad-block walk on an already-known
  `OPENWRT_UBI` layout;
- expanded `/api/status` with boot/recovery reason, FIP/FIT state, UBI counters,
  board/SoC/DRAM/FDT and update result;
- kept one normal operator `y/N` for the destructive transaction.

Historical artifact:

- FIP SHA256 `f164ff4ac0f272550151c4f0d89cd154bd7b0d6eb9165c44c469caec9fc0138f`;
- NT_FW margin before the first certificate: 2631 bytes.

The HWFIX3 provenance file names HWFIX2 as the hardware-proven persistent base.

#### HWFIX3

- suppressed the generic 3-second U-Boot autoboot countdown without saving env;
- cleaned Web UI recovery/previous-image/PEB wording;
- unified firmware and initramfs/FIT staging at `0x90000000`, up to 64 MiB;
- removed the obsolete 16 MiB expert staging window;
- made the two staging consumers invalidate each other's metadata explicitly.

Historical artifact:

- FIP SHA256 `e8809b69712b5c3e57d3230a9bcd86048f1e02a58106229c06a7be45a35b97a7`.

The UIFIX1 build contract describes HWFIX3 as hardware-tested.

#### UIFIX1

A deliberately narrow UI-only descendant of HWFIX3:

- normalized the NAND bad-block label to `BadBlocks`;
- kept UBI corrupted-PEB reporting separate;
- changed no boot/reset/NAND/UBI/network/migration/staging policy.

Historical artifact:

- FIP SHA256 `a6d1977eed9eb08bb9960b6621322fdd20babe07fb36fdb178b61dedc09a8ad4`.

### 0.1.0-alpha4-FUDAN1 — Fudan SPI-NAND support

FUDAN1 carried forward the UIFIX1 behavior and added the OpenWrt FMSH/Fudan support
line for FM25G01B/FM25G02B, including the Quad-I/O dummy-cycle correction.

Historical build contract:

- OpenWrt baseline `3d1645ee26d6a2e20be71d7fa1716721bac78e53`;
- U-Boot v2026.07;
- raw BL33 SHA256 `a41a81a011e19d1498d5ff0773dfd6bf9bd4c56beb3e7a46ce4622a643a30703`;
- FIP SHA256 `ce43b56d86321ccb7657d2e9b7ddf58e811efc73927855bbb75e896c83b18600`;
- NT_FW margin 3133 bytes.

Its BUILD_INFO explicitly marked Fudan hardware as unverified at that point and
required a SkyHigh regression. Therefore the historical build is **SOURCE/BUILD
evidence**, not a blanket Fudan **HW PASS**.

### 0.1.0-alpha5-UBIUX1 — OpenWrt-aware UBI recovery/update

Alpha5-UBIUX1 was the major persistent-recovery expansion on top of FUDAN1:

- allowed `OPENWRT_STOCK_LAYOUT -> OPENWRT_UBI` through the Recovery migration
  backend already used for Nokia stock;
- preserved BOSA/RI/FIP through migration and committed full BL2 last;
- added the Web "Keep OpenWrt settings" path for UBI updates;
- added "Reset OpenWrt settings" and the U-Boot command `ursussettings reset`;
- created fresh/reset `rootfs_data` with MAX minus 16 free PEBs and persisted
  `rootfs_data_max`;
- selected staged UBI update when headroom allowed it, otherwise a direct
  Recovery-safe replacement.

Historical artifact:

- raw BL33 SHA256 `06397f68ba876e01ba6a07ebbdbbcfac1e5341b9d82926fd6b4a54ae1bf7e552`;
- FIP SHA256 `548c446555231ee1b6ec4666000831226e0749c576d702c06dc5f501a6f510db`;
- NT_FW margin 1313 bytes.

The original BUILD_INFO classified this as source/build/package QA pending hardware
regression.

### TEST57 — diagnostics, transport recovery and Web transaction behavior

`0.1.0-alpha5-UBIUX1-TEST57` added the first structured safety/diagnostic layer
around the alpha5 path:

- fixed the `/api/reset-openwrt-settings` route-prefix off-by-one;
- extended API status with NAND geometry and structured failure/transaction state;
- added `/api/operation-log`;
- added host-side failure diagnostic bundles and full status snapshots;
- added bounded HTTP upload retry/reconcile using generation/received/total;
- removed the generic automatic TFTP fallback after a Web error;
- recorded the long pre-U-Boot UBI scan as a separate BL2 performance issue rather
  than changing layout during a safety regression.

The original changelog records the main stock -> UrsusBoot -> OpenWrt UBI ONE-CLICK
path as having passed on a SkyHigh S35ML02G300 during the TEST57 cycle. That
hardware result applies to that tested path, not automatically to every later
TEST57-derived change.

### TEST58 — diagnostic capture and reboot/wait correctness

TEST58 focused on host/recovery observability and state transitions:

- every write-capable operation captured before/after status, operation log, Web log,
  console snapshot and operation metadata;
- ONE-CLICK waited for the new UrsusBoot Recovery instance rather than mistaking the
  old stock HTTP service for a successful reboot;
- the wait UI stopped displaying negative time;
- build date/version identity was made deterministic with a fixed release epoch.

The main ONE-CLICK hardware path was inherited from TEST57; TEST58 itself required
focused regression of its new behavior.

### TEST59 — settings UX, reboot and UBI headroom correction

TEST59 introduced the corrective set later carried into TEST60/61:

- restored visibility of the keep/reset-settings decision for a new operation after a
  previous COMPLETE/FAILED transaction;
- allowed `/api/reboot` after ordinary UBI update, not only migration/self-update;
- made machine/UART project log strings English printable ASCII;
- replaced the artificial "restore absolute 16 free LEBs" update gate with projected
  headroom logic that preserves the headroom actually present before the operation.

At this point the old FIP packing budget was nearly exhausted: the historical TEST59
NT_FW ended only 30 bytes before the first certificate.

### TEST60 / CONFIGTRIM1 — recover FIP headroom without changing recovery policy

TEST60 removed U-Boot subsystems not used by the recovery product:

- `CONFIG_CMD_UBIFS=n`;
- PXE/extlinux command/bootmethod support disabled;
- UBI, TFTP, WGET and `bootcmd=ursusdispatch` retained.

This reduced raw BL33 from 956088 to 860176 bytes and compressed BL33 from 327650 to
291237 bytes, restoring 36443 bytes of NT_FW margin. Runtime WebFailsafe/update/reset
logic was intentionally unchanged from TEST59.

Evidence: source/build/package QA; focused CONFIGTRIM1 hardware regression was still
required.

### TEST61 / SAFETYREG1 — standalone baseline

TEST61 closed a set of safety regressions discovered while testing the combined
UrsusBoot/UrsusFlasher flow:

- fixed split build identity between native `version`, `.scmversion`, Web/API and
  host metadata;
- stopped ONE-CLICK from automatically performing a second FIP update after a direct
  stock `mtd0` write/readback had already proved the installed bootloader;
- explicit FIP self-update reuses the expected attached UBI rather than detaching it;
- increased upload reconnect grace and required a new operator `y/N` before a new
  full transfer cycle after failure;
- made pre-operation upload/validation failure explicitly `NOT_STARTED` and
  recoverable for the next attempt;
- kept direct-stock install to one meaningful `y/N` after backup/preflight;
- separated production TEST61 metadata from the pinned alpha3 emergency BootROM
  lineage;
- made public-test packaging reproducible through fixed epoch and canonical ordering;
- retained CONFIGTRIM1.

Known baseline artifact imported into the standalone repository:

- `ursusboot-md-0.1.0-alpha5-UBIUX1-TEST61-update.fip`
- size: 503808 bytes
- SHA256: `3c922e4256b6047376a7d445006e6cb2a4485bb412747033a77defd15e42fcea`

Historical raw U-Boot SHA256:

- `43296d98686ada9e4e13c5a5a49430372abf45e9bd0fc8eae837920e8ba5224d`

Historical LZMA payload SHA256:

- `bec245ab0b10e3fffcc2f0a482c2c3b97b03577b4a03c436857243cd68ff9d29`

TEST61 sizes recorded by the old changelog:

- raw BL33: 860808 bytes;
- LZMA BL33: 291160 bytes;
- FIP: 503808 bytes;
- NT_FW margin: 36520 bytes.

The persistent-FIP layout carried into the standalone packer is:

```text
NT_FW / BL33 offset     0x27800
first certificate      0x77800
checksum               0x7ac00
FIP logical end        0x7b000
physical end           0x7b800
```

The original TEST61 release status was source/build QA with mandatory hardware safety
regression for the TEST61-specific safety changes. The standalone project therefore
treats TEST61 as the **historical persistent baseline**, while continuing to label
new standalone behavior according to its own QA/BUILD/HW evidence.

### 2026-09-09 onward — AN7583 / XG-040G-MF development

The old `feature/mf-an7583` line introduced the first board-family abstraction and a
native AN7583 RAM recovery target:

- MD/MF `BoardProfile` applicability/writer policy;
- RAM-only MF2 with `ENV_IS_NOWHERE`, `bootcmd=ursusweb`, persistent Ursus writers
  rejected with `-EROFS`;
- exact AN7583 build/artifact audits and checks against MD identity leakage;
- family-aware stock/recovery routing;
- later MF persistent-runtime Kconfig/environment work and device-derived candidate
  tooling.

The 2026-09-09 checkpoint explicitly labelled MF2/HWTEST4 as **STATIC QA PASS /
HW-TEST CANDIDATE**, not hardware proof. GitHub Actions run `34360701468` produced
artifact `ursusboot-mf2-ram1-an7583-hwtest4`.

This distinction matters to the standalone profile today: XG-040G-MF has a real
AN7583 source/config lineage, but its persistent donor-FIP contract is still not
declared in the standalone registry.

### 2026-09-13/14 — XG-140G-MD experiments and modular Airoha architecture

The original development repository then explored XG-140G-MD and extracted the
multi-board architecture that later informed the standalone tree.

Selected modularization commits on `feature/ursusboot-modular-airoha`:

- `5a7d36132fbbb02e8965b0366a7fc93f6b95fbb0` — board profile registry;
- `7254076cb7645dd51ad314afdfb316a1b6fb0d3e` — generic profile resolver;
- `39f81a506fc8f3b475bfe8f9fe0de39460bca88f` — MD board policy;
- `50312bdc3978ff2f81a2dbb99b5369239b5a4868` — MF board policy;
- `3e0e751e299b9023346751aec86fbc02a35734cb` — XG140 board policy;
- `e338a4093eef1d78adfc16e5f740ad84efa29556` — dispatcher/StockBridge policy split;
- `1b63eb06728c90e55700390d69fc2a996a8d1463` through
  `0a671eb35453593665d13ebcdd3a39f7da1c6b10` — runtime-role declaration,
  resolution and source-level verification.

The XG140 line also proved an important negative design rule: sharing AN7581 does not
make MD boot geometry/FIP policy portable. XG140 therefore requires its own native
geometry/container/recovery proof.

The present standalone repository intentionally keeps XG140 as an incomplete profile
(`config`, template and donor FIP unset) rather than promoting experimental
UrsusFlasher-era assumptions into a flashable build.

### TRANSITION / Vanilla work is a separate product track

The old modular branch also contains a large TRANSITION/Vanilla history and handoff
documents. That work reused pieces of UrsusBoot (WebFailsafe, stock-slot tooling,
Airoha networking and recovery mechanics) while aiming at a final **non-Ursus**
OpenWrt boot chain.

It is useful engineering archaeology but is not part of the persistent standalone
UrsusBoot version line. In particular, TRANSITION2 experiments, pregnant initramfs,
Vanilla BL2/FIP migration and UnameOne production payload work must not be presented
as released standalone UrsusBoot features.

Historical sources consulted for this reconstruction include:

- `docs/CHANGELOG_RU.md` / `CHANGELOG_EN.md`;
- `docs/CHANGELOG_DEV_RU.md`;
- `docs/TEST57_TEST_RU.md` through `TEST61_TEST_RU.md`;
- alpha4/alpha5 `BUILD_INFO` files under `payloads/md/ursusboot/`;
- `docs/XG140_HANDOFF.md`;
- `docs/MD_VANILLA_HANDOFF.md`;
- modular-Airoha design/specification documents on
  `feature/ursusboot-modular-airoha`.

## Hardware validation policy

A changelog entry may contain multiple evidence levels. The strongest label applies only to the exact behavior/artifact actually tested.

For new boards or major boot/recovery changes, a useful HW record should include:

- board/model and revision if known;
- exact UrsusBoot commit SHA;
- exact FIP/install artifact SHA256;
- toolchain/build run identifier;
- UART log;
- NAND/bad-block status;
- MAC/network acceptance;
- recovery-path result.

Never include serial numbers, GPON credentials, live-device identity dumps or private backups in public test records.

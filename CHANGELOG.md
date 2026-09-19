# Changelog

This file records meaningful UrsusBoot milestones and, where relevant, the level of evidence behind them.

Evidence labels used here:

- **SOURCE** — source/configuration/documentation change exists in the repository.
- **QA PASS** — repository QA passed for the exact commit.
- **BUILD PASS** — the target build and packaging pipeline completed for the exact commit/run stated.
- **HW PASS** — the exact artifact/behavior was exercised successfully on real target hardware.

**QA PASS and BUILD PASS are not HW PASS.**

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

## Historical baseline — UrsusBoot 0.1.0-alpha5-UBIUX1-TEST61

The standalone repository uses the XG-040G-MD TEST61 lineage as its initial hardware-proven baseline.

Known baseline artifact:

- `ursusboot-md-0.1.0-alpha5-UBIUX1-TEST61-update.fip`
- size: 503808 bytes
- SHA256: `3c922e4256b6047376a7d445006e6cb2a4485bb412747033a77defd15e42fcea`

Historical raw U-Boot SHA256:

- `43296d98686ada9e4e13c5a5a49430372abf45e9bd0fc8eae837920e8ba5224d`

Historical LZMA payload SHA256:

- `bec245ab0b10e3fffcc2f0a482c2c3b97b03577b4a03c436857243cd68ff9d29`

The proven persistent-FIP layout used by the standalone packer is:

```text
NT_FW / BL33 offset     0x27800
first certificate      0x77800
checksum               0x7ac00
FIP logical end        0x7b000
physical end           0x7b800
```

TEST57-TEST60 development archaeology is intentionally not part of the supported standalone public build surface.

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

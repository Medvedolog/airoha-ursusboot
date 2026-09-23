# Testing and acceptance

UrsusBoot uses three different evidence levels. Keep them separate in issues, commits and releases.

## 1. QA PASS

Run:

```sh
bash scripts/qa.sh
```

QA is an offline structural check. It can prove invariants about repository contents and helper behavior, but it does not compile the target and does not exercise hardware.

## 2. Build PASS

A build PASS requires the exact target profile to compile with the target cross-toolchain and the packaging pipeline to finish successfully.

For XG-040G-MD that includes:

- current `u-boot.bin`;
- current `u-boot.lzma`;
- FIP repack;
- `FIP_CURRENT_BL33=PASS`;
- size/budget checks;
- 512 KiB install image;
- artifact hashes.

Record the exact commit SHA, workflow/run ID when CI is used, toolchain identity and artifact hash.

## 3. HW PASS

Only a real target can provide HW PASS.

### Network/WebFailsafe acceptance

With the device on an isolated direct recovery link:

1. cold boot with UART capture;
2. confirm WebFailsafe is reachable at the expected address;
3. verify `printenv ethaddr`;
4. verify the workstation's ARP/neighbor MAC matches;
5. run Web Console `ping`;
6. run UART `ping` while WebFailsafe remains active;
7. run TFTP while WebFailsafe remains active;
8. repeat network operations and confirm HTTP remains alive.

### MAC stability acceptance

Use full power removal between boots, not only the `reset` command.

On each of at least three cold boots:

```text
printenv ethaddr
```

On a Linux workstation:

```sh
ip neigh flush 192.168.1.1
ping -c2 192.168.1.1
ip neigh show 192.168.1.1
```

For a healthy RI-backed unit, expect:

```text
URSUS_MAC_SOURCE=RI ethaddr=...
```

The factory `ethaddr` must remain identical across cold boots and match the workstation's neighbor entry.

A U-Boot random-MAC warning on an empty environment can occur before `preboot`; it is not by itself failure if the later RI restoration is successful.

### FIP update / environment reset acceptance

After recording the stable factory MAC:

1. update UrsusBoot through WebFailsafe/UrsusFlasher or the tested TFTP route;
2. exercise the environment-reset path used by that update;
3. cold boot twice;
4. confirm the MAC equals the previously recorded factory value;
5. confirm RI restoration diagnostics are correct;
6. confirm WebFailsafe is reachable both times.

### Controller/env consistency check

After WebFailsafe is working, reboot from the web UI and reconnect **without manually clearing the workstation ARP/neighbor cache**.

The page should return normally. Failure here is evidence of a possible mismatch between the environment MAC/lwIP identity and the address programmed into the Ethernet controller.

### UART WebFailsafe stop/restart

While WebFailsafe is running:

1. press Ctrl-C on UART;
2. verify `URSUS_WEB_STOP_REQUEST source=UART`;
3. verify `URSUS_WEB_STOPPED restart=ursusweb`;
4. use standard networking at the U-Boot prompt;
5. run `ursusweb`;
6. verify HTTP returns.

### Firmware/install acceptance

For supported image classes:

- stage image;
- validate classification and hashes;
- confirm the expected layout transition/update path;
- execute the write;
- verify operation completion;
- verify readback;
- reboot manually;
- confirm the expected OpenWrt/stock boot result.

### TEST63 fast-BL2 / preloader acceptance

For each board (MD and MF), from Nokia stock:

- STOCK->UBI log shows `URSUS_UBI_PRELOADER_VALID ... sha256=<ubi_preloader_sha256>` and `URSUS_UBI_MIGRATION_BL2_VERIFIED ... sha256=<ubi_bl2_image_sha256>` matching `PROVENANCE.json`;
- readback of the BL2 area after BL2-LAST equals the 128 KiB candidate (same digest);
- the device then boots OpenWrt from UBI without Recovery; with UART, the BL2 log shows the fast scan path;
- time from power-on to OpenWrt noticeably shorter than with the previous preloader;
- WebFailsafe/`/api/status` reports `0.1.0-alpha5-t64`;
- MF: `URSUS_MAC_SOURCE=RI` on boot after migration (first HW check of the MF MAC path).

### TEST64 Vanilla replacement acceptance

After the TEST63/64 STOCK -> UBI migration, from UrsusBoot Recovery, per board:

- WebFailsafe shows "Pinned Vanilla FIP: yes"; uploading `vanilla-u-boot.fip` of the same release gives VALID and the log `URSUS_UBI_INSTALLED_BL2 OK`; any other FIP (an UrsusBoot FIP, the other board's Vanilla FIP) is REJECTED;
- the replacement logs `URSUS_UPDATE_ARMED kind=VANILLA layout=UBI`, `URSUS_VANILLA_ENV_RESET_OK`, `URSUS_UPDATE_COMMIT_OK layout=UBI kind=VANILLA backup=fip.old` and `URSUS_VANILLA_REPLACE_COMPLETE`;
- after a power cycle the UART shows the fast BL2 and `U-Boot 2026.07` without `UrsusBoot`, and OpenWrt boots from UBI;
- in OpenWrt the UBI volume `fip` hashes to `vanilla_fip_sha256` of PROVENANCE.json (read exactly the FIP length) and `fip.old` still holds the UrsusBoot FIP.

## Reporting a test

A useful hardware report includes:

- board/model;
- board revision if known;
- exact UrsusBoot commit;
- exact FIP/install artifact SHA256;
- toolchain/build run identifier;
- UART log;
- observed MAC;
- NAND/bad-block status;
- which acceptance items passed or failed.

Do not include serial, GPON credentials, customer/device identity dumps or private backups in public reports.

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

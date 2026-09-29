# Handoff — streaming HTTP backup and read catalog

**Branch:** `dev/ursusboot-http-backup` (from `test75-tftpput` / `1b30854c`)
**Version:** `0.1.0-alpha5-t79` (t77 plus small console fixes, see CHANGELOG;
the streaming backup below is unchanged from t77)
**Status:** t77: SOURCE + repo QA + BUILD PASS (both boards). t78: SOURCE only
until built. No hardware for either.
t76 (`2b14dd8c`) compiled and fit on both boards but has a re-entrancy defect;
see below. **Do not use t76 for backups.**

## Why this line exists

UrsusFlasher's F5 NAND presets dump through `mtd read` → RAM → `tftpput` → PC,
orchestrated over the WebSocket console. For a 256 MiB chip that is 32 round
trips of 8 MiB, a TFTP receiver on the operator's PC, a UDP port through their
firewall, and a hard precondition of 128 MiB verified DRAM for two staging
buffers.

Reading **pbs05** [`uboot-an758x`](https://github.com/pbs05/uboot-an758x) showed
a simpler shape: its `net/lwip/httpd.c:fs_read_custom()` streams the backup
straight out of flash into the HTTP response, one eraseblock at a time, with no
RAM staging at all. Bad eraseblocks are emitted as `0xFF` to keep physical
offsets — the same decision UrsusFlasher's archive format already made.

So the plan is not to copy their feature onto our heavier transport. It is to
take the streaming idea, which removes the transport question entirely.

pbs05 is credited in `docs/CREDITS.md` as an independent parallel
implementation, not a source of UrsusBoot. That stands: this is the same idea,
written against our own HTTP server.

## What is in this branch

```text
src/u-boot/cmd/ursusdl.inc     the catalog, both streams, the main-loop service
src/u-boot/cmd/ursusweb.c      conn state, deferred teardown, route, main loop
scripts/qa.sh                  T76 guards (they guard the t77 design)
```

Three read-only endpoints, all under `GET /api/backup/`:

```text
catalog                      live NAND MTD devices and, if attached, UBI volumes
mtd/<name>[?offset=&size=]   a live MTD device or partition, main area
ubi/<volume>                 one UBI volume, whole
```

`catalog` returns JSON built from the live lists:

```json
{"schema":1,
 "mtd":[{"name":"spi-nand0","root":"spi-nand0","offset":0,"size":268435456,
         "erase":131072,"page":2048,"whole":true},
        {"name":"ursus-ubi-full","root":"spi-nand0","offset":524288,"size":...,
         "erase":131072,"page":2048,"whole":false}],
 "ubi":{"attached":true,"leb":126976,
        "volumes":[{"name":"fip","id":2,"type":"static","size":...}]}}
```

`offset` is absolute in the `root` device, found by walking the parent chain
(the kernel's own `mtd->offset` is relative to the parent). An archive records
physical offsets, so a client can turn "partition X" into
`GET /api/backup/mtd/<root>?offset=&size=` without a console and without parsing
`mtd list`. The catalog does no flash I/O: it runs inside a callback, and the
QA guard checks that. Resume works the same way, at eraseblock granularity: ask
for the remaining `offset`/`size`. There is no HTTP `Range` support.

Guards, all before a single byte is sent:

```text
names are [A-Za-z0-9_.-], shorter than 64
the device or volume exists in the live lists
MTD range aligned to erasesize and inside the device (wrap-safe comparison)
UBI volume has no interrupted-update marker
no UBI migration, UBI update or FIP update is active
no other stream is already running
```

Refused while the live WebSocket console is connected: `ursus_ws_is_active()`
rejects every other HTTP request, these included. A client must close the
console before downloading.

## The design, and why it is not the obvious one

The obvious design — refill the body from flash inside the lwIP send callback,
so TCP backpressure paces the dump — is **wrong here**, and t76 shipped it.

Since t74, `board_schedule_poll()` pumps lwIP from `schedule()`, which the
SPI-NAND driver calls between pages. A flash read started inside a callback can
therefore be interrupted by a *nested* callback on the same connection:

- a nested `tcp_sent` runs the pump, which starts a second refill on the same
  buffer and the same offset. The outer read then advances the offset again and
  a whole span is skipped: a backup that is silently wrong;
- a nested `tcp_err` or close (a peer reset arriving mid-read — the ordinary
  way a long download ends) frees the connection and its buffer while the outer
  `mtd_read` is still writing into it.

So t77 reads flash from one place only, `ursus_dl_service()`, called from the
main loop next to `ursus_console_service_pending()`. The callbacks only queue
and acknowledge. A teardown that lands while a read owns the connection
(`dl_busy`) marks it `dl_dead` and returns; the read finishes the teardown.
`scripts/qa.sh` asserts that no callback can read flash and that both teardown
paths defer.

Other behaviour worth knowing:

- 16 KiB per pass, never across an eraseblock; `tcp_write()` copies, so the
  buffer is reusable as soon as a span is queued.
- A read error aborts the connection. A truncated download is a broken
  transfer, never a complete-looking short file.
- UBI volumes are opened and closed around each read, never held between
  passes. `ubi detach` from the expert console is not refused while a volume is
  open, so a kept descriptor could dangle. The device pointer and the volume's
  size are re-checked every pass; a change aborts the stream.
- No change to `cmd/ubi.c`: `ubi_read()` is used directly, so the per-call
  `Read N bytes from volume` line never appears.
- 60 s without a new span aborts the stream, and state-changing requests are
  locked out only while flash is still being read.
- The expert console (`POST /api/console`) is deliberately not locked during a
  stream; it is the real U-Boot command line, and an operator who erases the
  flash under a running dump gets what they asked for.

## What this is not

- **Not a forensic dump.** Main area only, ECC-corrected. No OOB, no OTP, and a
  bad eraseblock's real content is replaced by `0xFF`.
- **Not a write path.** Nothing here erases, writes, maps or unmaps. Restore
  keeps going through the existing verified path.
- **Not a stock partition table for MF.** The catalog lists what U-Boot has as
  MTD devices. On a Nokia stock layout that is the DTS partitions (`bl2`,
  `ubi`), not the named stock partitions, which do not exist as MTD devices
  there. What this *does* give on MF stock: the whole chip, and any range of it
  with `offset`/`size`. Named per-partition dumps on MF stock still need a
  proven physical map from somewhere else. An earlier note in this line said the
  catalog "closes the MF gap"; it closes the *read-any-range* gap, not the
  *named partitions* gap.

## Found in review

Defects caught by reading rather than by a build, all before a device ran this:

- **Client reset leaked the MTD device** (t76). `ursus_http_err` freed the
  connection without releasing the backup. Fixed, and superseded by the deferred
  teardown below.
- **The range check could wrap** (t76). `offset + size > mtd->size` adds two u64
  values from the query string; an aligned `size` near 2^64 wraps and passes.
  Now `size > mtd->size - offset`.
- **Re-entrancy from `board_schedule_poll()`** (t76 → t77): the section above.
  Found while reading the t74 yield path to decide how to stream UBI volumes.
  It would have been the worst kind of defect here: a backup that finishes with
  the right length and the wrong bytes.
- **A stalled peer could lock the operator out** (t77 draft). One stream that
  stopped acknowledging would have held `ursus_dl_active()` and refused every
  POST, including reboot, until TCP gave up. Bounded by the 60 s stall abort.

The QA guards match code with comments stripped: an early range guard tripped on
a comment that quoted the old expression.

## Measured cost

Build run `36539542258` on t77 (`92857c50`), against t76 (run `36492426186`) and
the t75 baseline (run `36299597450`), BL33 compressed size in bytes:

```text
board       t75      t76      t77     t77-t76   t77-t75   headroom t77
MD       269256   270314   272184     +1870     +2928     54.2 KiB (83.1% used)
MF       270157   270938   272649     +1711     +2492     58.7 KiB (81.9% used)
```

The whole line -- catalog, MTD and UBI streaming, the main-loop service and the
deferred teardown -- costs about 2.9 KiB on MD and 2.5 KiB on MF compressed, about
5% of the headroom t75 had. Moving the read out of the callbacks and adding the
catalog and UBI path cost roughly 1.7-1.9 KiB more than the t76 endpoint alone.
Every number is from `check_bl33_budget.py` on the exact run, not an estimate.

## Next steps, in order

1. **Build t77 and read the budget** -- done, see above. `build.yml` also
   triggers on this branch (temporary; the filter carries a comment to restore
   `[main, 'test*']` before merge). A full run takes 30-45 min because the
   toolchain is built from source.
2. **Host side** (UrsusFlasher, `dev/ursusboot-http-backup-client`): one HTTP
   request per dump, resume by `offset`/`size`, discovery from the catalog, no
   UDP/1069 on the read direction. Two things still need the WebSocket console
   and therefore cannot overlap the download: the bad-block list (`mtd bad`;
   computing it in the catalog would mean flash I/O inside a callback) and an
   independent spot-check of the downloaded file against U-Boot's own
   `mtd read` + `hash sha256`. Restore keeps using TFTP.
3. **Hardware acceptance**, in this order because each step is cheaper than the
   next: catalog on a UBI layout and on a stock layout; a small MTD range;
   a UBI volume; a whole-chip dump with a `sha256sum` cross-check against
   `mtd read` + `hash` over the same span; then interrupt a dump from the client
   side (Ctrl-C, closed tab, pulled cable) and confirm the router still answers,
   the device is free, and a second dump starts. The last one is the test that
   exercises the deferred-teardown path this whole design exists for.

## Not planned

`nand-scrub` / "Clear bad block markers". pbs05 has it (`erase.scrub=1`,
commits `9a1b700c`, `6428d82c`). It force-erases blocks the manufacturer marked
bad, destroying that marking irreversibly; where the markers were correct the
result is silent corruption later. If it is ever wanted it needs its own
confirmation, its own document and its own hardware acceptance — not a button
next to backup.

## Evidence

```text
scripts/qa.sh   PASS (URSUSBOOT_STANDALONE_QA=PASS, URSUSBOOT_PIPELINE_QA=PASS)
T76/T77 guards  ten mutations, every one caught: a callback reading flash,
                either teardown path losing the busy deferral, a UBI descriptor
                held between passes, the wrapping range check, a write call in
                the backup path, the refill outside dl_busy, the stall abort
                removed, the catalog losing the parent walk, flash I/O in the
                catalog
build           t76 BUILD PASS, run 36492426186 on 2b14dd8c (superseded)
                t77 BUILD PASS, run 36539542258 on 92857c50, both boards
                MD artifact 11021240150, MF artifact 11022310688
BL33 budget     MD 272184 / 327680 (54.2 KiB free, 83.1% used)
                MF 272649 / 332800 (58.7 KiB free, 81.9% used)
hardware        none
```

BUILD PASS proves it compiles, links and fits; it says nothing about behaviour.
No HW PASS is claimed and no endpoint here has ever served a byte.

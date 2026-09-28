# Handoff — streaming HTTP backup

**Branch:** `dev/ursusboot-http-backup` (from `test75-tftpput` / `1b30854c`)
**Version:** `0.1.0-alpha5-t76`
**Status:** SOURCE + repo QA + BUILD PASS (both boards). No hardware.

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

One endpoint, plus the plumbing that lets the existing response pump produce a
body from flash instead of from a fixed buffer.

```text
src/u-boot/cmd/ursusdl.inc     new; the whole streaming reader
src/u-boot/cmd/ursusweb.c      conn state, pump refill, release hook, route
```

`GET /api/backup/mtd/<name>[?offset=&size=]`

```text
name     a live MTD device or partition, as `mtd list` names it
offset   default 0, must be eraseblock aligned
size     default to the end of the device, must be eraseblock aligned
```

Guards, all before a single byte is sent:

```text
name is [A-Za-z0-9_.-], shorter than 64
device exists in the live MTD list
range aligned to erasesize and inside the device
no UBI migration, UBI update or FIP update is active
```

Behaviour worth knowing:

- The body is refilled 16 KiB at a time and never across an eraseblock, so each
  lwIP callback does one short NAND read and TCP backpressure paces the dump.
- `tcp_write()` copies, so refilling the staging buffer under a queued segment
  is safe.
- A read error returns `ERR_VAL` from the pump, which aborts the connection.
  A truncated download is therefore a broken transfer, never a complete-looking
  short file.
- `Content-Length` is the requested span, so progress works in a browser.

## What this is not

- **Not a forensic dump.** Main area only, ECC-corrected. No OOB, no OTP, and a
  bad eraseblock's real content is replaced by `0xFF`.
- **Not a write path.** Nothing here erases or writes. Restore keeps going
  through the existing verified path.
- **Not UBI volumes yet.** `ubi_volume_read()` prints a line per call
  (`cmd/ubi.c`), so a 40 MiB volume at 32 KiB per read would put ~1280 lines on
  the UART. pbs05 added a `_quiet` variant for exactly this. Deferred on
  purpose: MTD streaming already covers whole-chip and per-partition dumps, and
  the first thing this branch owes is a size measurement, not a second feature.

## Found in review of the first commit

Two defects in the first push (`2c1689b6`), both caught by reading rather than by
a build, and both fixed before any device ran this code:

- **Client reset leaked the MTD device.** `ursus_http_err` (the lwIP `tcp_err`
  path) freed the connection without calling `ursus_dl_release`, so closing the
  browser tab or hitting Ctrl-C on `curl` mid-dump left `get_mtd_device_nm()`'s
  reference held and the 16 KiB buffer lost. That is the *ordinary* way a
  256 MiB download ends early, so it would have happened on the first real use.
- **The range check could wrap.** `offset + size > mtd->size` adds two u64 values
  taken from the query string; an aligned `size` near 2^64 wraps the sum to a
  small number and passes. Now `size > mtd->size - offset`, safe because
  `offset < mtd->size` is already established.

The QA guards match code with comments stripped: the first version of the range
guard tripped on a comment that quoted the old expression.

## Next steps, in order

1. **Measure — done.** Build run `36492426186` on `2b14dd8c` compiled both
   boards with the pinned OpenWrt gcc 14.4 toolchain and ran
   `check_bl33_budget.py`. The cost of this endpoint against the t75 baseline
   (run `36299597450`, `1b30854c`):

   ```text
   board  BL33 LZMA t75  BL33 LZMA t76   delta    headroom t75 -> t76
   MD          269256         270314     +1058 B   57.1 KiB -> 56.0 KiB
   MF          270157         270938      +781 B   61.2 KiB -> 60.4 KiB
   ```

   About 1 KiB compressed, roughly 1.8% of the remaining headroom on either
   board, so the line continues. The pre-build guess was "~60 lines"; the code
   is ~200 lines including guards and comments, and it still costs only about
   a kilobyte after LZMA. The guess about *size* held; the guess about *lines*
   did not, and the number above is the one to rely on.
2. **If it fits:** generic read catalog. UrsusBoot currently offers no
   per-partition dump on an MF stock layout because no proven physical map
   exists for it. For *reading* that caution buys nothing — reading a wrong
   range is harmless — so the catalog can come from the live MTD/UBI lists and
   the restriction can stay on the write side only. That closes the MF gap.
3. **Then:** UBI volume streaming, with a quiet read variant.
4. **Host side:** point UrsusFlasher's F5 backup at this endpoint. It collapses
   to one HTTP request with `Range` support, which also makes resume free and
   retires the UDP/1069 question for the read direction.

## Not planned

`nand-scrub` / "Clear bad block markers". pbs05 has it (`erase.scrub=1`,
commits `9a1b700c`, `6428d82c`). It force-erases blocks the manufacturer marked
bad, destroying that marking irreversibly; where the markers were correct the
result is silent corruption later. If it is ever wanted it needs its own
confirmation, its own document and its own hardware acceptance — not a button
next to backup.

## Evidence

```text
scripts/qa.sh      PASS (URSUSBOOT_STANDALONE_QA=PASS, URSUSBOOT_PIPELINE_QA=PASS)
T76 guards         read-only path, wrap-safe range check, MTD released on both
                   teardown paths (normal release and the tcp_err reset path),
                   read error aborts, 0xFF for bad blocks, refused mid-operation;
                   each verified to fail when its fix is reverted
build              BUILD PASS, run 36492426186 on 2b14dd8c, both boards
                   MD artifact 11003014132, MF artifact 11003138301
BL33 budget        MD 270314 / 327680 (56.0 KiB free, 82.5% used)
                   MF 270938 / 332800 (60.4 KiB free, 81.4% used)
hardware           none
```

BUILD PASS proves it compiles, links and fits; it says nothing about behaviour.
No HW PASS is claimed and the endpoint has never served a byte.

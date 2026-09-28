# Handoff — streaming HTTP backup

**Branch:** `dev/ursusboot-http-backup` (from `test75-tftpput` / `1b30854c`)
**Version:** `0.1.0-alpha5-t76`
**Status:** SOURCE + repo QA. No build, no hardware.

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

## Next steps, in order

1. **Measure.** `build.yml` now also triggers on this branch (temporary — the
   filter carries a comment saying to restore `[main, 'test*']` before merge).
   `scripts/ci/build-release.sh` ends in `scripts/ci/check_bl33_budget.py`,
   which prints the BL33 headroom for MD and MF. Those two numbers decide
   whether this line continues. At t73 the diet left roughly 60 KiB on MD and
   68 KiB on MF; the estimate for this change was ~60 lines of code, and the
   estimate has not been checked against a compiler yet.
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
build              not run in this session — no cross toolchain in the container
hardware           none
```

No BUILD PASS and no HW PASS are claimed. The endpoint has never served a byte.

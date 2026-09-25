#!/usr/bin/env python3
"""BL33 size budget in the Nokia stock bootloader window (mtd0, 512 KiB).

UrsusBoot lives entirely in BL33 and, on the Nokia stock layout, must fit the
stock FIP window 0x800..0x7C000 (ROM prefix before it, Nokia env at 0x7C000).

  MD: the persistent FIP is repacked from the donor (repack_persistent_fip.py):
      NT_FW (BL33) starts at the donor's offset, the 0x3800-byte certificate/
      checksum tail follows 0x800-aligned, and 0x800 + FIP end <= 0x7BFFC.
  MF: the stock-layout FIP is derived from the device's own mtd0; only BL33 is
      replaced in place (UrsusFlasher mf_persistent.py). On the Nokia
      XG-040G-MF stock FIP, NT_FW starts at 0x2A400 and the FIP must end by
      0x7B800 (0x7C000 - 0x800).

Prints the headroom, emits a GitHub warning below --warn bytes and fails when
the image does not fit.  Usage: check_bl33_budget.py <board> <dist-dir>
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src/u-boot"))
import repack_persistent_fip as rp  # noqa: E402

PHYS_OFF = 0x800
MD_PHYS_LIMIT = 0x7BFFC          # repack_persistent_fip.py hard limit
MF_STOCK_NT_OFF = 0x2A400        # Nokia XG-040G-MF stock FIP, NT_FW offset
MF_FIP_MAX = 0x7C000 - PHYS_OFF  # FIP must end before the stock env


def md_budget(dist: Path) -> tuple[int, int]:
    s, f, entries, tp, end = rp.parse((dist / "ursusboot-update.fip").read_bytes())
    nt = next(e for e in entries if e["uuid"] == rp.NT_UUID)
    used_end = nt["off"] + nt["size"]
    # largest 0x800-aligned certificate start that still satisfies the limit
    max_first = (MD_PHYS_LIMIT - PHYS_OFF - rp.END_REL) // rp.ALIGN * rp.ALIGN
    return nt["size"], max_first - nt["off"]


def mf_budget(dist: Path) -> tuple[int, int]:
    size = (dist / "u-boot.runtime.lzma").stat().st_size
    return size, MF_FIP_MAX - MF_STOCK_NT_OFF


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("board", choices=("xg040-md", "xg040-mf"))
    ap.add_argument("dist", type=Path)
    ap.add_argument("--warn", type=int, default=16 * 1024)
    a = ap.parse_args()
    used, limit = (md_budget if a.board == "xg040-md" else mf_budget)(a.dist)
    free = limit - used
    print(f"BL33_BUDGET board={a.board} bl33_lzma={used} limit={limit} "
          f"headroom={free} ({free / 1024:.1f} KiB, {100 * used / limit:.1f}% used)")
    if free < 0:
        print(f"::error::{a.board}: BL33 does not fit the Nokia stock bootloader window "
              f"(over by {-free} bytes)")
        return 1
    if free < a.warn:
        print(f"::warning::{a.board}: BL33 headroom in the stock window is only {free} bytes "
              f"({free / 1024:.1f} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

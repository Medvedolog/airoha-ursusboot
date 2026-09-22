#!/usr/bin/env python3
"""Wrap a raw Airoha BL2 (<soc>-bl2.bin) into the UBI preloader FIP form.

The output is byte-identical to `fiptool create --tb-fw <bl2>`: one TB_FW entry
followed by the terminator entry and the payload. This is the form UrsusBoot's
STOCK->UBI migration accepts (ursus_ubi_validate_preloader) and places at 0x800
of the 128 KiB BL2 area. Wrapping the BL2 of the HW-proven MD preloader
reproduces it exactly (6c3b2339...).
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

FIP_TOC_NAME = 0xAA640001
FIP_TOC_SERIAL = 0x12345678
TB_FW_UUID = bytes.fromhex("5ff9ec0b4d223e4da544c39d81c73f0a")
MAX_PRELOADER = 0x20000 - 0x800  # must fit the 128 KiB BL2 candidate after the 0x800 FF prefix


def is_fip(blob: bytes) -> bool:
    return len(blob) >= 16 and struct.unpack_from("<I", blob, 0)[0] == FIP_TOC_NAME


def wrap_tb_fw(bl2: bytes) -> bytes:
    off = 16 + 2 * 40
    return (
        struct.pack("<IIQ", FIP_TOC_NAME, FIP_TOC_SERIAL, 0)
        + TB_FW_UUID + struct.pack("<QQQ", off, len(bl2), 0)
        + bytes(16) + struct.pack("<QQQ", off + len(bl2), 0, 0)
        + bl2
    )


def tb_fw_payload(fip: bytes) -> bytes:
    """Return the BL2 of a single-entry TB_FW FIP, rejecting anything else."""
    if not is_fip(fip):
        raise ValueError("not a FIP")
    entries, pos = [], 16
    while True:
        uuid = fip[pos:pos + 16]
        off, size, _flags = struct.unpack_from("<QQQ", fip, pos + 16)
        pos += 40
        if uuid == bytes(16):
            break
        entries.append((uuid, off, size))
    if len(entries) != 1 or entries[0][0] != TB_FW_UUID:
        raise ValueError(f"expected exactly one TB_FW entry, got {len(entries)}")
    _uuid, off, size = entries[0]
    if off + size > len(fip):
        raise ValueError("TB_FW entry out of bounds")
    return fip[off:off + size]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bl2", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    raw = Path(args.bl2).read_bytes()
    if not raw or is_fip(raw):
        raise SystemExit("expected a raw, non-empty BL2 image")
    fip = wrap_tb_fw(raw)
    if len(fip) > MAX_PRELOADER:
        raise SystemExit(f"preloader FIP {len(fip)} bytes exceeds {MAX_PRELOADER}")
    assert tb_fw_payload(fip) == raw
    Path(args.out).write_bytes(fip)
    print(f"PRELOADER_FIP={args.out} size={len(fip)} sha256={hashlib.sha256(fip).hexdigest()} "
          f"bl2_sha256={hashlib.sha256(raw).hexdigest()}")


if __name__ == "__main__":
    main()

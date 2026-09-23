#!/usr/bin/env python3
"""Pin the one Vanilla OpenWrt U-Boot FIP this UrsusBoot may install.

cmd/ursusupdate.c carries `ursus_vanilla_fip_sha256[32]`, all zero by default
(no Vanilla FIP pinned: /api/replace-with-vanilla and `ursusupdate
vanilla-write` refuse). This rewrites exactly that array with the SHA256 of
the given FIP. It refuses to re-pin a tree that already carries a digest.
"""
from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

ARRAY = re.compile(r"(static const u8 ursus_vanilla_fip_sha256\[32\] = \{)([^}]*)(\};)")
FIP_MAGIC = 0xAA640001


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tree", type=Path, required=True, help="U-Boot tree (work/<board>-<role>/u-boot)")
    ap.add_argument("--fip", type=Path, required=True, help="Vanilla bl31-uboot FIP")
    args = ap.parse_args()

    data = args.fip.read_bytes()
    if len(data) < 16 or int.from_bytes(data[:4], "little") != FIP_MAGIC:
        raise SystemExit(f"{args.fip}: not a FIP")
    digest = hashlib.sha256(data).hexdigest()
    path = args.tree / "cmd" / "ursusupdate.c"
    text = path.read_text(encoding="utf-8")
    hits = ARRAY.findall(text)
    if len(hits) != 1:
        raise SystemExit(f"{path}: expected one ursus_vanilla_fip_sha256 array, found {len(hits)}")
    body = hits[0][1]
    if any(int(tok, 16) for tok in re.findall(r"0x([0-9a-fA-F]{2})", body)):
        raise SystemExit(f"{path}: a Vanilla FIP digest is already pinned; refusing to re-pin")
    rows = [", ".join(f"0x{digest[i + j:i + j + 2]}" for j in range(0, 16, 2)) for i in range(0, 64, 16)]
    new_body = "\n    " + ",\n    ".join(rows) + "\n"
    text = ARRAY.sub(lambda m: m.group(1) + new_body + m.group(3), text, count=1)
    path.write_text(text, encoding="utf-8")
    print(f"VANILLA_FIP_PIN file=cmd/ursusupdate.c")
    print(f"VANILLA_FIP_SHA256={digest}")
    print(f"VANILLA_FIP_SIZE={len(data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

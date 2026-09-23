#!/usr/bin/env python3
"""Turn an OpenWrt uboot-airoha .config into the RECOVERY_SAFE RAM U-Boot config.

Same contract as MedveFlasher RC18: the default environment is exactly
scripts/recovery/rcsafe_env (no autoboot, marker medveflasher_recovery_safe=rc18),
and the saved environment is looked up in UBI volumes that never exist
(RCSAFE00/RCSAFE002), so no environment left on NAND by UrsusBoot, Vanilla or
OpenWrt can re-enable autoboot.  `--check` verifies an olddefconfig'd .config.
"""
from __future__ import annotations

import sys
from pathlib import Path

WANT = {
    # U-Boot >= 2023.07 names (OpenWrt 3d1645ee ships U-Boot 2026.07).
    "CONFIG_ENV_USE_DEFAULT_ENV_TEXT_FILE": "y",
    "CONFIG_ENV_DEFAULT_ENV_TEXT_FILE": '"defenvs/ursus_rcsafe_env"',
    "CONFIG_ENV_UBI_VOLUME": '"RCSAFE00"',
    "CONFIG_ENV_UBI_VOLUME_REDUND": '"RCSAFE002"',
    "CONFIG_BOOTDELAY": "-1",
}


def parse(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


def main() -> int:
    check = "--check" in sys.argv[1:]
    path = Path([a for a in sys.argv[1:] if a != "--check"][0])
    text = path.read_text()
    if check:
        cur = parse(text)
        bad = {k: cur.get(k) for k, v in WANT.items() if cur.get(k) != v}
        if "CONFIG_ENV_IS_IN_UBI" in cur and cur["CONFIG_ENV_IS_IN_UBI"] != "y":
            bad["CONFIG_ENV_IS_IN_UBI"] = cur["CONFIG_ENV_IS_IN_UBI"]
        if bad:
            raise SystemExit(f"RECOVERY_SAFE config not applied: {bad}")
        print("RCSAFE_CONFIG=PASS " + " ".join(f"{k}={v}" for k, v in WANT.items()))
        return 0
    # Pre-2023.07 names would be dropped by olddefconfig; never leave them behind.
    stale = ("CONFIG_USE_DEFAULT_ENV_FILE", "CONFIG_DEFAULT_ENV_FILE")
    lines = []
    seen = set()
    for line in text.splitlines():
        key = line.split("=", 1)[0] if line.startswith("CONFIG_") else (
            line[2:].split(" ", 1)[0] if line.startswith("# CONFIG_") else None)
        if key in stale:
            continue
        if key in WANT:
            if key not in seen:
                lines.append(f"{key}={WANT[key]}")
                seen.add(key)
            continue
        lines.append(line)
    lines += [f"{k}={v}" for k, v in WANT.items() if k not in seen]
    path.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Write dist/<board>/PROVENANCE.json for a release build (scripts/ci/build-release.sh).

UrsusFlasher accepts a release only if PROVENANCE.json names the pinned commit and
every listed file matches its digest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--board", required=True)
    ap.add_argument("--openwrt-ref", required=True)
    ap.add_argument("--openwrt-patches-from", required=True, type=Path, help="config/fast-bl2.json")
    ap.add_argument("--atf-source", required=True)
    ap.add_argument("--atf-patch", required=True, type=Path)
    ap.add_argument("--atf-upstream", required=True)
    ap.add_argument("--uboot-variant", required=True, help="OpenWrt uboot-airoha variant of the Vanilla FIP")
    a = ap.parse_args()
    out = a.out
    pre = (out / "ursusboot-ubi-preloader.fip").read_bytes()
    cand = b"\xff" * 0x800 + pre
    cand += b"\xff" * (0x20000 - len(cand))
    info = dict(l.split("=", 1) for l in (out / "BUILD-INFO.txt").read_text().splitlines() if "=" in l)
    if info.get("UBI_PRELOADER_SHA256") != hashlib.sha256(pre).hexdigest():
        raise SystemExit("BUILD-INFO pin does not match the packaged preloader")
    vanilla = (out / "vanilla-u-boot.fip").read_bytes()
    if info.get("VANILLA_FIP_SHA256") != hashlib.sha256(vanilla).hexdigest():
        raise SystemExit("BUILD-INFO Vanilla pin does not match the packaged Vanilla FIP")
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet", "HEAD", "--"], check=False).returncode != 0
    prov = {
        "schema": 1,
        "repo": "Medvedolog/airoha-ursusboot",
        "commit": commit,
        "tree_dirty": dirty,
        "version": (ROOT / "VERSION").read_text().strip(),
        "board": a.board,
        "openwrt_ref": a.openwrt_ref,
        "openwrt_patches": {p: sha(ROOT / p) for p in json.loads(a.openwrt_patches_from.read_text()).get("openwrt_patches", [])},
        "atf_source_version": a.atf_source,
        "atf_patch_sha256": sha(a.atf_patch),
        "atf_patch_upstream": a.atf_upstream,
        "ubi_preloader_sha256": hashlib.sha256(pre).hexdigest(),
        "ubi_bl2_image_sha256": hashlib.sha256(cand).hexdigest(),
        "vanilla_fip_sha256": hashlib.sha256(vanilla).hexdigest(),
        "vanilla_uboot_variant": a.uboot_variant,
        "files": {p.name: sha(p) for p in sorted(out.iterdir()) if p.is_file() and p.name != "PROVENANCE.json"},
        "hw_status": "HW_PENDING",
    }
    if dirty:
        raise SystemExit("refusing to write release provenance for a dirty working tree")
    (out / "PROVENANCE.json").write_text(json.dumps(prov, indent=2) + "\n")
    print(json.dumps({k: v for k, v in prov.items() if k != "files"}, indent=2))


if __name__ == "__main__":
    main()

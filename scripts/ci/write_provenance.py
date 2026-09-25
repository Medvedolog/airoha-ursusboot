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
    update_fip = out / "ursusboot-update.fip"
    if not update_fip.is_file():
        raise SystemExit("canonical ursusboot-update.fip is missing from dist")
    if info.get("URSUSBOOT_UPDATE_FIP_SHA256") != sha(update_fip):
        raise SystemExit("BUILD-INFO repair FIP digest does not match the packaged ursusboot-update.fip")
    rs = json.loads((out / "RECOVERY-SAFE-FIP-REPACK.json").read_text())
    persistent_report_path = out / "MF-PERSISTENT-FIP-REPACK.json"
    persistent = json.loads(persistent_report_path.read_text()) if persistent_report_path.is_file() else None
    if persistent is not None:
        if persistent.get("output_sha256") != sha(update_fip) or persistent.get("bl31_byte_exact") is not True \
                or persistent.get("mf2_bl33_roundtrip") is not True or persistent.get("mf2_bl33_lzma_eopm") is not False:
            raise SystemExit("MF persistent repair FIP report does not match the packaged ursusboot-update.fip")
    if rs.get("output_sha256") != sha(out / "recovery-safe-u-boot.fip") or rs.get("bl31_byte_exact") is not True \
            or rs.get("mf2_bl33_roundtrip") is not True or rs.get("mf2_bl33_lzma_eopm") is not False:
        raise SystemExit("RECOVERY_SAFE FIP report does not match the packaged FIP")
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
        "ursusboot_update_fip_sha256": sha(update_fip),
        "ursusboot_update_fip_size": update_fip.stat().st_size,
        "persistent_fip_donor_sha256": info.get("PERSISTENT_REPAIR_DONOR_SHA256",
                                                  info.get("DONOR_FIP_SHA256")),
        # BootROM/UART RAM recovery (RC18 RECOVERY_SAFE contract, Fudan-capable).
        "recovery_safe_fip_sha256": sha(out / "recovery-safe-u-boot.fip"),
        "recovery_safe_fip_size": (out / "recovery-safe-u-boot.fip").stat().st_size,
        "recovery_safe_bl31_sha256": rs["bl31_compressed_sha256"],
        "recovery_safe_bl33_sha256": rs["mf2_bl33_compressed_sha256"],
        "recovery_safe_donor_sha256": rs["source_sha256"],
        "files": {p.name: sha(p) for p in sorted(out.iterdir()) if p.is_file() and p.name != "PROVENANCE.json"},
        "hw_status": "HW_PENDING",
    }
    if persistent is not None:
        prov.update({
            "persistent_fip_bl31_sha256": persistent["bl31_compressed_sha256"],
            "persistent_fip_bl33_sha256": persistent["mf2_bl33_compressed_sha256"],
            "persistent_fip_bl33_raw_sha256": persistent["mf2_bl33_raw_sha256"],
        })
    if dirty:
        raise SystemExit("refusing to write release provenance for a dirty working tree")
    (out / "PROVENANCE.json").write_text(json.dumps(prov, indent=2) + "\n")
    print(json.dumps({k: v for k, v in prov.items() if k != "files"}, indent=2))


if __name__ == "__main__":
    main()

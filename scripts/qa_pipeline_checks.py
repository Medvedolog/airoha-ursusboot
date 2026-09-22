#!/usr/bin/env python3
"""Python checks used by scripts/qa-pipeline.sh."""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


def _imports(root: Path):
    sys.path.insert(0, str(root / "scripts" / "atf"))
    sys.path.insert(0, str(root / "scripts"))
    import pin_ubi_preloader as pin  # noqa: E402
    import wrap_bl2_preloader as wrap  # noqa: E402
    return pin, wrap


def profiles(root: Path) -> None:
    _pin, wrap = _imports(root)
    reg = json.loads((root / "config/board-profiles.json").read_text())

    def sha(rel: str) -> str:
        return hashlib.sha256((root / rel).read_bytes()).hexdigest()

    # MF reference inputs are pinned by provenance (reference/mf/PROVENANCE.txt).
    assert sha("reference/mf/medve-rc35-mf-recovery-safe-bl31-uboot.fip") == \
        "8bfe8870e44923a463a3ed66c8b1906214f5c820fd8c15865c63430185de8bb2"
    assert sha("reference/mf/medve-rc35-mf-uart-preloader.bin") == \
        "c2ac1c183b18bc34632c958dfe0bd1dfdfb607f090e39c41126956641893362f"
    cfg = json.loads((root / "config/fast-bl2.json").read_text())
    assert (root / cfg["atf_patch"]).is_file() and len(cfg["openwrt_ref"]) == 40
    for name in ("xg040-md", "xg040-mf"):
        p = reg["profiles"][name]
        for k in ("packaging", "preloader_pin_base", "openwrt_device", "config_require",
                  "binary_require", "reference_fip"):
            assert p.get(k), (name, k)
        for m in ("FM25G01B", "FM25G02B"):  # Fudan SPI-NAND support must stay compiled in
            assert m in p["binary_require"], (name, m)
        assert "CONFIG_MTD_SPI_NAND" in p["config_require"], name
        for frags in p.get("role_fragments", {}).values():
            for f in frags:
                assert (root / "config" / f).is_file(), (name, f)
        for ov in p.get("env_overlay", {}).values():
            assert (root / ov["src"]).is_file(), (name, ov)
        if p.get("uart_preloader"):
            assert (root / p["uart_preloader"]).is_file(), name
    assert "URSUS_MAC_SOURCE=RI" in (root / "config/an7583_nokia_xg-040g-mf_RUNTIME_env").read_text()
    bl2 = bytes(range(256)) * 400
    fip = wrap.wrap_tb_fw(bl2)
    assert wrap.is_fip(fip) and wrap.tb_fw_payload(fip) == bl2
    print("pipeline profiles/provenance/wrap: PASS")


def pin(root: Path, tmp: Path) -> None:
    """The pin replaces exactly one array per digest and refuses to pin twice."""
    pinmod, wrap = _imports(root)
    tree = tmp / "pin"
    (tree / "cmd").mkdir(parents=True)
    (tree / "include").mkdir(parents=True)
    src = (root / "src/u-boot/cmd/ursusubi.c").read_bytes()
    (tree / "cmd/ursusubi.c").write_bytes(src)
    (tree / "include/ursus_ubi.h").write_bytes((root / "src/u-boot/include/ursus_ubi.h").read_bytes())
    pre = wrap.wrap_tb_fw(b"QA-FAST-BL2" * 9000)
    counts = pinmod.pin(tree, pre, pinmod.MD)
    assert counts["preloader_sha_bytes"] == 1 and counts["bl2_sha_bytes"] == 1, counts
    text = (tree / "cmd/ursusubi.c").read_text()
    assert pinmod.MD["preloader_sha"] not in text and pinmod.MD["bl2_sha"] not in text
    assert hashlib.sha256(pre).hexdigest() in text
    try:
        pinmod.pin(tree, pre, pinmod.MD)
    except SystemExit:
        pass
    else:
        raise SystemExit("re-pinning an already pinned tree did not fail")
    print("UBI preloader pin selftest: PASS")


def transforms(root: Path, trees: Path) -> None:
    pinmod, _wrap = _imports(root)
    md = (trees / "md/cmd/ursusubi.c").read_text()
    mf = (trees / "mf/cmd/ursusubi.c").read_text()
    assert pinmod.MD["preloader_sha"] in md
    assert pinmod.MF["preloader_sha"] in mf and pinmod.MD["preloader_sha"] not in mf, "MF digest swap missing"
    assert "URSUS_BOARD_PROFILE_MARKER" in (trees / "md/cmd/ursusdispatch.c").read_text()
    assert "URSUS_BOARD_PROFILE_MARKER" in (trees / "mf/cmd/ursusdispatch.c").read_text()
    print("MD/MF source transforms + board policy: PASS")


if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    {"profiles": profiles, "pin": pin, "transforms": transforms}[cmd](*map(Path, args))

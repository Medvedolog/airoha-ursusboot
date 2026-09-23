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
    # The Vanilla U-Boot needs Fudan FM25G02B in uboot-airoha (openwrt/openwrt PR 24025).
    assert cfg["openwrt_patches"] == ["scripts/openwrt/openwrt-pr24025-uboot-fmsh-fm25g02b.patch"], cfg
    assert sha(cfg["openwrt_patches"][0]) == "a3e843e60c7efdf6f103c04153c40b711a369962a533f370ffbc38aa0dcf314e"
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
        assert p.get("uboot_variant"), (name, "uboot_variant")  # TEST64 Vanilla FIP source
        hdr = (root / "boards" / p["board_policy_header"]).read_text()
        assert f'URSUS_BOARD_COMPATIBLE             "{p["compatible"]}"' in hdr, name
        assert "URSUS_BOARD_OTHER_COMPATIBLE" in hdr and f'OTHER_COMPATIBLE       "{p["compatible"]}"' not in hdr, name
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


def vanilla(root: Path, tmp: Path) -> None:
    """TEST64: Vanilla FIP pin (default none, one array, no re-pin) and builder (both donors)."""
    import lzma, re, struct, subprocess
    src = (root / "src/u-boot/cmd/ursusupdate.c").read_text()
    arr = re.findall(r"ursus_vanilla_fip_sha256\[32\] = \{([^}]*)\}", src)
    assert len(arr) == 1 and not any(int(t, 16) for t in re.findall(r"0x([0-9a-f]{2})", arr[0])), "source must ship unpinned"
    body = src[src.index("static int ursus_vanilla_fip_validate_buf"):src.index("static int ursus_fip_validate_kind")]
    assert "nokia,xg-040g" not in body and "an758" not in body, "Vanilla validator must use board-policy macros only"
    work = tmp / "vanilla"
    (work / "tree/cmd").mkdir(parents=True)
    (work / "tree/cmd/ursusupdate.c").write_text(src)
    raw = b"U-Boot 2026.07 (OpenWrt) FM25G02B nokia,xg-040g-md airoha,an7581 " + bytes(range(256)) * 400
    nt = work / "nt.lzma"
    nt.write_bytes(lzma.compress(raw, format=lzma.FORMAT_ALONE))
    mk = root / "scripts/vanilla/make_vanilla_fip.py"
    def build(donor, board, other, out):
        return subprocess.run([sys.executable, str(mk), "--donor", str(root / donor), "--nt-fw", str(nt), "--output",
                               str(out), "--board", board, "--other-board", other, "--require-marker", "U-Boot 2026.07",
                               "--forbid-marker", "UrsusBoot-"], capture_output=True, text=True)
    r = build("reference/md/ursusboot-test61-update.fip", "nokia,xg-040g-md", "nokia,xg-040g-mf", work / "md.fip")
    assert r.returncode == 0 and "NOKIA_STOCK_FORMAT" in r.stdout, r.stdout + r.stderr
    r = build("reference/mf/medve-rc35-mf-recovery-safe-bl31-uboot.fip", "nokia,xg-040g-md", "nokia,xg-040g-mf", work / "mf.fip")
    assert r.returncode == 0 and "SOC_FW+NT_FW" in r.stdout, r.stdout + r.stderr
    r = build("reference/md/ursusboot-test61-update.fip", "nokia,xg-040g-mf", "nokia,xg-040g-md", work / "bad.fip")
    assert r.returncode != 0, "Vanilla builder accepted another board's U-Boot"
    def entries(b):
        out, pos = {}, 16
        while b[pos:pos + 16] != b"\0" * 16:
            off, size, _f = struct.unpack_from("<QQQ", b, pos + 16)
            out[b[pos:pos + 16].hex()] = b[off:off + size]
            pos += 40
        return out
    a = entries((work / "md.fip").read_bytes())
    d = entries((root / "reference/md/ursusboot-test61-update.fip").read_bytes())
    changed = sorted(k for k in a if a[k] != d[k])
    assert changed == ["a2cceab7f8254b279704633a6fd69ad8", "d6d0eea7fcead54b97829934f234b6e4"], changed
    pinpy = root / "scripts/pin_vanilla_fip.py"
    run = lambda: subprocess.run([sys.executable, str(pinpy), "--tree", str(work / "tree"), "--fip", str(work / "md.fip")],
                                 capture_output=True, text=True)
    r = run()
    assert r.returncode == 0, r.stderr
    digest = hashlib.sha256((work / "md.fip").read_bytes()).hexdigest()
    pinned = (work / "tree/cmd/ursusupdate.c").read_text()
    assert ", ".join(f"0x{digest[i:i + 2]}" for i in range(0, 16, 2)) in pinned
    assert run().returncode != 0, "pin_vanilla_fip re-pinned"
    print("vanilla FIP builder (MD stock-format, MF two-entry, wrong board) + pin: PASS")


def transforms(root: Path, trees: Path) -> None:
    pinmod, _wrap = _imports(root)
    md = (trees / "md/cmd/ursusubi.c").read_text()
    mf = (trees / "mf/cmd/ursusubi.c").read_text()
    assert pinmod.MD["preloader_sha"] in md
    assert pinmod.MF["preloader_sha"] in mf and pinmod.MD["preloader_sha"] not in mf, "MF digest swap missing"
    assert "URSUS_BOARD_PROFILE_MARKER" in (trees / "md/cmd/ursusdispatch.c").read_text()
    assert "URSUS_BOARD_PROFILE_MARKER" in (trees / "mf/cmd/ursusdispatch.c").read_text()
    for b in ("md", "mf"):  # TEST64 Vanilla replacement survives both derivations
        upd = (trees / b / "cmd/ursusupdate.c").read_text()
        web = (trees / b / "cmd/ursusweb.c").read_text()
        ui = (trees / b / "include/ursusweb_ui.inc").read_text(encoding="latin-1")
        assert "ursus_vanilla_fip_validate" in upd and "URSUS_BOARD_OTHER_COMPATIBLE" in upd, b
        assert "ursus_ubi_installed_bl2_matches_pin" in upd, b
        # t65: Vanilla must not inherit UrsusBoot's saved environment (HW t64: stopped at bootmenu).
        assert "ursus_vanilla_reset_env()" in upd and "URSUS_VANILLA_ENV_RESET_OK" in upd, b
        assert "/api/replace-with-vanilla " in web and "REPLACE-URSUSBOOT-WITH-VANILLA" in web, b
        assert "vanilla-fip-begin" in ui and "replaceVanilla" in ui, b
    print("MD/MF source transforms + board policy + Vanilla replacement: PASS")


def recovery_safe(root: Path, tmp: Path) -> None:
    """BootROM/UART RECOVERY_SAFE RAM U-Boot: RC18 contract, pinned donors, packing."""
    import hashlib
    import json
    import subprocess

    def sha(rel: str) -> str:
        return hashlib.sha256((root / rel).read_bytes()).hexdigest()

    reg = json.loads((root / "config/board-profiles.json").read_text())["profiles"]
    donors = {
        "xg040-md": ("reference/md/rc18-md-recovery-safe-bl31-uboot-ethfix.fip",
                     "2ebcbf3981e3e56b6389521fc2caa3320cf259c08f173b660b29366b9290bcc1"),
        "xg040-mf": ("reference/mf/medve-rc35-mf-recovery-safe-bl31-uboot.fip",
                     "8bfe8870e44923a463a3ed66c8b1906214f5c820fd8c15865c63430185de8bb2"),
    }
    for board, (rel, digest) in donors.items():
        assert reg[board].get("recovery_safe_donor") == rel, (board, reg[board].get("recovery_safe_donor"))
        assert sha(rel) == digest, (board, rel)
    # Exactly the RC18 default environment: no autoboot, marker for the host gates.
    env = (root / "scripts/recovery/rcsafe_env").read_text().splitlines()
    assert env == ["bootdelay=-1", "bootcmd=echo RECOVERY_SAFE_RC18", "preboot=echo RECOVERY_SAFE_RC18",
                   "medveflasher_recovery_safe=rc18"], env
    cfg = tmp / "rcsafe.config"
    cfg.write_text('CONFIG_ENV_IS_IN_UBI=y\nCONFIG_ENV_UBI_VOLUME="ubootenv"\n'
                   'CONFIG_ENV_UBI_VOLUME_REDUND="ubootenv2"\n# CONFIG_ENV_USE_DEFAULT_ENV_TEXT_FILE is not set\n'
                   'CONFIG_USE_DEFAULT_ENV_FILE=y\n'
                   'CONFIG_BOOTDELAY=0\n')
    tool = str(root / "scripts/recovery/rcsafe_config.py")
    subprocess.run([sys.executable, tool, str(cfg)], check=True)
    subprocess.run([sys.executable, tool, "--check", str(cfg)], check=True, stdout=subprocess.DEVNULL)
    assert "ubootenv" not in cfg.read_text() and "CONFIG_USE_DEFAULT_ENV_FILE" not in cfg.read_text()
    # Both donors repack with the RC18 encoder: BL31 byte-exact, known size, no EOPM.
    raw = tmp / "rcsafe-standin.bin"
    raw.write_bytes(b"U-Boot 2026.07 RECOVERY_SAFE stand-in " * 4096)
    for board, (rel, _digest) in donors.items():
        rep = tmp / f"{board}-rs.json"
        subprocess.run([sys.executable, str(root / "scripts/mf/mf2_repack_from_medve.py"),
                        "--medve-patcher", str(root / "scripts/mf/medve/patch_recovery_safe_fip.py"),
                        "--source", str(root / rel), "--bl33-raw", str(raw),
                        "--bl33-output", str(tmp / f"{board}-rs.lzma"), "--output", str(tmp / f"{board}-rs.fip"),
                        "--report", str(rep)], check=True, stdout=subprocess.DEVNULL)
        r = json.loads(rep.read_text())
        assert r["bl31_byte_exact"] and r["mf2_bl33_roundtrip"] and r["mf2_bl33_lzma_known_size"], board
        assert r["mf2_bl33_lzma_eopm"] is False and r["entry_count"] == 2, board
    build = (root / "scripts/ci/build-release.sh").read_text()
    for needle in ("rcsafe_config.py", "--check", "recovery_safe_donor", "FM25G02B",
                   "medveflasher_recovery_safe=rc18", "RCSAFE00", "recovery-safe-u-boot.fip"):
        assert needle in build, needle
    print("RECOVERY_SAFE UART RAM U-Boot (RC18 env, pinned donors, RC18 packing): PASS")


if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    {"profiles": profiles, "pin": pin, "vanilla": vanilla, "transforms": transforms, "recovery-safe": recovery_safe}[cmd](*map(Path, args))

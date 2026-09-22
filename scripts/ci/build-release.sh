#!/usr/bin/env bash
# Release build of one board: fast-scan BL2 -> preloader FIP -> UrsusBoot pinned
# to that preloader -> packaging -> provenance. Used by CI here and by
# airoha-router-ursusflasher (which checks out this repository at an exact SHA).
#
#   OPENWRT_DIR=/path/to/openwrt ./scripts/ci/build-release.sh <board>
#
# OPENWRT_DIR must be an official OpenWrt checkout at config/fast-bl2.json
# "openwrt_ref". Its toolchain builds both the BL2 and the UrsusBoot BL33.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD="${1:?usage: build-release.sh <board>}"
: "${OPENWRT_DIR:?Set OPENWRT_DIR to an official OpenWrt checkout}"
OPENWRT_DIR="$(cd "$OPENWRT_DIR" && pwd)"
CFG="$ROOT/config/fast-bl2.json"
cfg() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$CFG" "$1"; }
prof() {
    python3 "$ROOT/scripts/resolve_board_profile.py" --registry "$ROOT/config/board-profiles.json" \
        --profile "$BOARD" --json | python3 -c 'import json,sys; v=json.load(sys.stdin).get(sys.argv[1]); print("" if v is None else v)' "$1"
}
SOC="$(prof soc)"
DEVICE="$(prof openwrt_device)"
[ -n "$DEVICE" ] || { echo "board $BOARD: profile has no openwrt_device (fast BL2 not defined)" >&2; exit 3; }
WANT_REF="$(cfg openwrt_ref)"
GOT_REF="$(git -C "$OPENWRT_DIR" rev-parse HEAD)"
[ "$GOT_REF" = "$WANT_REF" ] || { echo "OpenWrt at $GOT_REF, config/fast-bl2.json requires $WANT_REF" >&2; exit 3; }
PATCH="$ROOT/$(cfg atf_patch)"
OUTBL2="$ROOT/work/$BOARD-fast-bl2"
rm -rf "$OUTBL2"; mkdir -p "$OUTBL2"

# --- OpenWrt target, host tools and toolchain -------------------------------
cd "$OPENWRT_DIR"
cat > .config <<EOF
CONFIG_TARGET_airoha=y
CONFIG_TARGET_airoha_${SOC}=y
CONFIG_TARGET_airoha_${SOC}_DEVICE_${DEVICE}=y
EOF
make defconfig
make download -j8
find dl -type f -size -1024c -delete
make -j"$(nproc)" tools/install V=s || make -j1 tools/install V=s
make -j"$(nproc)" toolchain/install V=s || make -j1 toolchain/install V=s

# --- fast BL2: patch hooked into Build/Prepare so every extraction is patched -
git checkout -- package/boot/arm-trusted-firmware-airoha/Makefile
python3 "$ROOT/scripts/atf/hook_atf_fastpath_patch.py" --openwrt . --patch "$PATCH"
make package/boot/arm-trusted-firmware-airoha/clean
make -j"$(nproc)" package/boot/arm-trusted-firmware-airoha/compile V=s 2>&1 | tee "$OUTBL2/atf.log" \
    || make -j1 package/boot/arm-trusted-firmware-airoha/compile V=s 2>&1 | tee -a "$OUTBL2/atf.log"
BL2DIR="$(find build_dir -maxdepth 2 -type d -name "trusted-firmware-a-${SOC}-bl2" -print -quit)"
[ -n "$BL2DIR" ] || { echo "BL2 build dir not found" >&2; exit 4; }
SRC="$(find "$BL2DIR" -path '*/plat/ecnt/en7523/bl2_boot_nand_ubi.c' -print -quit)"
[ -n "$SRC" ] || { echo "BL2 UBI source not found" >&2; exit 4; }
grep -Fq nandflash_read_range "$SRC"
grep -Fq nandflash_read_oob "$(dirname "$SRC")/../common/drivers/flash/spi_nand_flash.c"
grep -Fq 'patching file plat/ecnt/en7523/bl2_boot_nand_ubi.c' "$OUTBL2/atf.log"
BL2="staging_dir/target-aarch64_cortex-a53_musl/image/${SOC}-bl2.bin"
[ -s "$BL2" ] && [ "$BL2" -nt "$SRC" ] || { echo "BL2 missing or older than patched source" >&2; exit 4; }
cp "$BL2" "$OUTBL2/${SOC}-bl2.bin"
python3 "$ROOT/scripts/atf/wrap_bl2_preloader.py" --bl2 "$OUTBL2/${SOC}-bl2.bin" --out "$OUTBL2/preloader.fip"
ATF_SRC="$(sed -n 's/^[[:space:]]*SOURCE_VERSION:=//p' package/boot/arm-trusted-firmware-airoha/Makefile | head -n1)"
git checkout -- package/boot/arm-trusted-firmware-airoha/Makefile

# --- UrsusBoot pinned to exactly this preloader ------------------------------
cd "$ROOT"
OPENWRT_SDK="$OPENWRT_DIR" URSUS_UBI_PRELOADER="$OUTBL2/preloader.fip" ./build.sh "$BOARD"
OUT="$ROOT/dist/$BOARD"
cp "$OUTBL2/preloader.fip" "$OUT/ursusboot-ubi-preloader.fip"
cp "$OUTBL2/${SOC}-bl2.bin" "$OUT/${SOC}-bl2.bin"
python3 - "$OUT" "$BOARD" "$(cat VERSION)" "$(git rev-parse HEAD)" "$WANT_REF" "$ATF_SRC" "$PATCH" "$(cfg atf_patch_upstream)" <<'PY'
import hashlib, json, sys
from pathlib import Path
out, board, version, commit, owrt, atf_src, patch, upstream = sys.argv[1:]
out = Path(out)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
pre = (out / "ursusboot-ubi-preloader.fip").read_bytes()
cand = b"\xff" * 0x800 + pre
cand += b"\xff" * (0x20000 - len(cand))
info = dict(l.split("=", 1) for l in (out / "BUILD-INFO.txt").read_text().splitlines() if "=" in l)
if info.get("UBI_PRELOADER_SHA256") != hashlib.sha256(pre).hexdigest():
    raise SystemExit("BUILD-INFO pin does not match the packaged preloader")
prov = {
    "schema": 1,
    "repo": "Medvedolog/airoha-ursusboot",
    "commit": commit,
    "version": version,
    "board": board,
    "openwrt_ref": owrt,
    "atf_source_version": atf_src,
    "atf_patch_sha256": sha(patch),
    "atf_patch_upstream": upstream,
    "ubi_preloader_sha256": hashlib.sha256(pre).hexdigest(),
    "ubi_bl2_image_sha256": hashlib.sha256(cand).hexdigest(),
    "files": {p.name: sha(p) for p in sorted(out.iterdir()) if p.is_file() and p.name != "PROVENANCE.json"},
    "hw_status": "HW_PENDING",
}
(out / "PROVENANCE.json").write_text(json.dumps(prov, indent=2) + "\n")
print(json.dumps({k: v for k, v in prov.items() if k != "files"}, indent=2))
PY
echo "URSUSBOOT_RELEASE=OK board=$BOARD"

#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"; BOARD="${1:-xg040-md}"; ROLE="${2:-persistent}"; SDK="${OPENWRT_SDK:-${3:-}}"
case "$BOARD" in
  xg040-md) TEMPLATE="$ROOT/boards/nokia-xg040-md/stock-mtd0-template.bin"; CONFIG="$ROOT/config/u-boot.TEST61.full.config" ;;
  xg040-mf) TEMPLATE="$ROOT/boards/nokia-xg040-mf/stock-mtd0-template.bin"; CONFIG="$ROOT/config/an7583_nokia_xg-040g-mf_MF2_RAM_defconfig" ;;
  *) echo "usage: OPENWRT_SDK=/path/to/sdk ./build.sh {xg040-md|xg040-mf} [persistent|ram-recovery]" >&2; exit 2 ;;
esac
: "${SDK:?Set OPENWRT_SDK to an extracted OpenWrt SDK/toolchain}"
export STAGING_DIR="$SDK/staging_dir"
CROSS="$(find "$STAGING_DIR" -type f \( -name 'aarch64-openwrt-linux-musl-gcc' -o -name 'aarch64-openwrt-linux-gcc' \) | head -1 || true)"; [ -n "$CROSS" ] || { echo 'AArch64 compiler not found in OpenWrt SDK' >&2; exit 3; }; export CROSS_COMPILE="${CROSS%gcc}"
cp "$CONFIG" "$ROOT/src/u-boot/.config"; make -C "$ROOT/src/u-boot" olddefconfig; make -C "$ROOT/src/u-boot" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
mkdir -p "$ROOT/dist/$BOARD"; cp "$ROOT/src/u-boot/u-boot.bin" "$ROOT/dist/$BOARD/u-boot.bin"
FIP="${URSUS_FIP:-}"; if [ -z "$FIP" ] && [ "$BOARD" = xg040-md ]; then FIP="$ROOT/reference/md/ursusboot-test61-update.fip"; fi
if [ -n "$FIP" ] && [ -f "$FIP" ]; then python3 "$ROOT/scripts/make-install-mtd0.py" --template "$TEMPLATE" --fip "$FIP" --output "$ROOT/dist/$BOARD/ursusboot-install-mtd0.bin"; fi

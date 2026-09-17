#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
REGISTRY="$ROOT/config/board-profiles.json"
RESOLVE="$ROOT/scripts/resolve_board_profile.py"
BOARD="${1:-xg040-md}"
ROLE="${2:-}"
SDK="${OPENWRT_SDK:-${3:-}}"
role_args=()
[ -n "$ROLE" ] && role_args=(--role "$ROLE")
profile_field() {
    python3 "$RESOLVE" --registry "$REGISTRY" --profile "$BOARD" ${role_args[@]+"${role_args[@]}"} --field "$1"
}
ROLE="$(profile_field runtime_role)"
CONFIG="$ROOT/config/$(profile_field config)"
TEMPLATE="$ROOT/$(profile_field boot_area_template)"
[ -f "$CONFIG" ] || { echo "board $BOARD: missing config $CONFIG" >&2; exit 3; }
[ -f "$TEMPLATE" ] || { echo "board $BOARD: missing boot-area template $TEMPLATE" >&2; exit 3; }
: "${SDK:?Set OPENWRT_SDK to an extracted OpenWrt SDK/toolchain}"
export STAGING_DIR="$SDK/staging_dir"
export STAGING_DIR_HOST="${STAGING_DIR_HOST:-$STAGING_DIR/host}"
CROSS="$(find "$STAGING_DIR" -type f \( -name 'aarch64-openwrt-linux-musl-gcc' -o -name 'aarch64-openwrt-linux-gcc' \) | head -1 || true)"
[ -n "$CROSS" ] || { echo 'AArch64 compiler not found in OpenWrt SDK' >&2; exit 3; }
export CROSS_COMPILE="${CROSS%gcc}"
python3 "$ROOT/scripts/apply_runtime_role.py" "$ROOT/src/u-boot" --role "$ROLE" || {
    echo "runtime role $ROLE could not be applied; restore the tree with:" >&2
    echo "  git checkout -- src/u-boot/defenvs src/u-boot/include" >&2
    exit 4
}
cp "$CONFIG" "$ROOT/src/u-boot/.config"
make -C "$ROOT/src/u-boot" olddefconfig
make -C "$ROOT/src/u-boot" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
OUT="$ROOT/dist/$BOARD"
mkdir -p "$OUT"
cp "$ROOT/src/u-boot/u-boot.bin" "$OUT/u-boot.bin"
DONOR="${URSUS_FIP:-}"
if [ -z "$DONOR" ]; then
    REF="$(profile_field reference_fip 2>/dev/null || true)"
    [ -n "$REF" ] && DONOR="$ROOT/$REF"
fi
if [ -n "$DONOR" ]; then
    [ -f "$DONOR" ] || { echo "board $BOARD: FIP reference/donor not found: $DONOR" >&2; exit 5; }
    HOSTTOOLS="$ROOT/dist/.host-tools"
    mkdir -p "$HOSTTOOLS"
    cc -O2 -Wall -Wextra "$ROOT/scripts/lzma1ext_noeopm.c" -llzma -o "$HOSTTOOLS/lzma1ext_noeopm"
    "$HOSTTOOLS/lzma1ext_noeopm" "$OUT/u-boot.bin" "$OUT/u-boot.lzma" 1048576
    python3 "$ROOT/scripts/repack-fip.py" --donor "$DONOR" --bl33 "$OUT/u-boot.lzma" --output "$OUT/ursusboot-update.fip"
    python3 "$ROOT/scripts/make-install-mtd0.py" --template "$TEMPLATE" --fip "$OUT/ursusboot-update.fip" --output "$OUT/ursusboot-install-mtd0.bin"
else
    echo "URSUSBOOT_FIP=SKIP board=$BOARD reason=no-reference-fip"
fi
echo "URSUSBOOT_BUILD=OK board=$BOARD role=$ROLE out=dist/$BOARD"

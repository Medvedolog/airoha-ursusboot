#!/usr/bin/env bash
# Build UrsusBoot for one board profile and, when a reference FIP lineage is
# available, emit a current-source FIP plus a ready-to-flash 512 KiB mtd0 image.
#
#   OPENWRT_SDK=/path/to/sdk-or-toolchain ./build.sh [board] [runtime-role] [sdk]
#
# Board inputs and runtime roles come from config/board-profiles.json.
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
    python3 "$RESOLVE" --registry "$REGISTRY" --profile "$BOARD" \
        ${role_args[@]+"${role_args[@]}"} --field "$1"
}

ROLE="$(profile_field runtime_role)"
CONFIG_NAME="$(profile_field config)"
TEMPLATE_NAME="$(profile_field boot_area_template)"
CONFIG="$ROOT/config/$CONFIG_NAME"
TEMPLATE="$ROOT/$TEMPLATE_NAME"
[ -f "$CONFIG" ] || { echo "board $BOARD: missing config $CONFIG" >&2; exit 3; }
[ -f "$TEMPLATE" ] || { echo "board $BOARD: missing boot-area template $TEMPLATE" >&2; exit 3; }

: "${SDK:?Set OPENWRT_SDK to an extracted official OpenWrt SDK/toolchain}"
[ -d "$SDK" ] || { echo "OpenWrt SDK/toolchain directory not found: $SDK" >&2; exit 3; }

# Support both full OpenWrt SDKs (with staging_dir/) and official standalone
# OpenWrt toolchain bundles (where toolchain-* is the package root).
if [ -d "$SDK/staging_dir" ]; then
    export STAGING_DIR="$SDK/staging_dir"
    SEARCH_ROOT="$STAGING_DIR"
else
    SEARCH_ROOT="$SDK"
    export STAGING_DIR="$SDK"
fi

CROSS="$(find "$SEARCH_ROOT" -type f \
    \( -name 'aarch64-openwrt-linux-musl-gcc' -o -name 'aarch64-openwrt-linux-gcc' \) | head -1 || true)"
[ -n "$CROSS" ] || { echo "AArch64 OpenWrt compiler not found under $SDK" >&2; exit 3; }
export CROSS_COMPILE="${CROSS%gcc}"

# Prefer OpenWrt host tools when a full SDK supplies them; standalone toolchain
# bundles use normal host tools from PATH.
HOST_DIR=""
if [ -d "$SDK/staging_dir" ]; then
    HOST_DIR="$(find "$SDK/staging_dir" -maxdepth 1 -type d -name 'host*' | head -1 || true)"
fi
if [ -n "$HOST_DIR" ] && [ -d "$HOST_DIR/bin" ]; then
    export STAGING_DIR_HOST="$HOST_DIR"
    export PATH="${CROSS%/*}:$HOST_DIR/bin:$PATH"
    [ -d "$HOST_DIR/share/bison" ] && export BISON_PKGDATADIR="$HOST_DIR/share/bison"
else
    export PATH="${CROSS%/*}:$PATH"
fi

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

REF="${URSUS_FIP_TEMPLATE:-${URSUS_FIP:-}}"
if [ -z "$REF" ]; then
    REF_NAME="$(profile_field reference_fip 2>/dev/null || true)"
    [ -n "$REF_NAME" ] && REF="$ROOT/$REF_NAME"
fi

if [ -n "$REF" ]; then
    [ -f "$REF" ] || { echo "board $BOARD: reference FIP not found: $REF" >&2; exit 5; }
    command -v gcc >/dev/null || { echo 'host gcc is required for LZMA1EXT packaging' >&2; exit 5; }

    HOST_LZMA="$OUT/lzma1ext_noeopm"
    gcc -O2 -Wall -Wextra "$ROOT/src/u-boot/lzma1ext_noeopm.c" -llzma -o "$HOST_LZMA"
    "$HOST_LZMA" "$OUT/u-boot.bin" "$OUT/u-boot.lzma" 1048576

    FIP_OUT="$OUT/ursusboot-update.fip"
    python3 "$ROOT/src/u-boot/repack_persistent_fip.py" \
        "$REF" "$OUT/u-boot.lzma" "$FIP_OUT"

    python3 - "$FIP_OUT" "$OUT/u-boot.lzma" <<'PY'
import hashlib, struct, sys
from pathlib import Path
fip = Path(sys.argv[1]).read_bytes()
want = Path(sys.argv[2]).read_bytes()
nt_uuid = bytes.fromhex('d6d0eea7fcead54b97829934f234b6e4')
if struct.unpack_from('<I', fip, 0)[0] != 0xaa640001:
    raise SystemExit('generated FIP magic mismatch')
pos = 16
found = None
while pos + 40 <= len(fip):
    uid = fip[pos:pos+16]
    if uid == b'\0' * 16:
        break
    off, size, flags = struct.unpack_from('<QQQ', fip, pos + 16)
    if uid == nt_uuid:
        found = fip[off:off+size]
        break
    pos += 40
if found is None:
    raise SystemExit('generated FIP has no NT_FW/BL33 entry')
if found != want:
    raise SystemExit('generated FIP BL33 does not match current u-boot.lzma')
if len(fip) >= 0x7b800:
    raise SystemExit(f'generated FIP exceeds stock budget: {len(fip):#x}')
print('FIP_CURRENT_BL33=PASS')
print('FIP_SHA256=' + hashlib.sha256(fip).hexdigest())
print('BL33_LZMA_SHA256=' + hashlib.sha256(want).hexdigest())
PY

    python3 "$ROOT/scripts/make-install-mtd0.py" \
        --template "$TEMPLATE" --fip "$FIP_OUT" \
        --output "$OUT/ursusboot-install-mtd0.bin"
else
    echo "board $BOARD: no reference_fip declared; raw u-boot.bin only" >&2
fi

sum_files=("$OUT/u-boot.bin")
if [ -n "$REF" ]; then
    sum_files+=("$OUT/u-boot.lzma" "$OUT/ursusboot-update.fip" "$OUT/ursusboot-install-mtd0.bin")
fi
sha256sum "${sum_files[@]}" | tee "$OUT/SHA256SUMS"

echo "URSUSBOOT_BUILD=OK board=$BOARD role=$ROLE out=dist/$BOARD"

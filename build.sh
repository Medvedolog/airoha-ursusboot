#!/usr/bin/env bash
# Build UrsusBoot for one board profile.
#
#   OPENWRT_SDK=/path/to/sdk ./build.sh <board> [role]
#
# The build never mutates src/u-boot: it works on a clean copy under
# work/<board>-<role>/u-boot and applies, in order, the profile's source
# transforms, runtime role, board policy, version identity and (optionally)
# the STOCK->UBI preloader pin. Outputs go to dist/<board>/.
#
# Optional environment:
#   URSUS_UBI_PRELOADER  preloader FIP UrsusBoot must accept for STOCK->UBI
#                        (e.g. a fast-scan BL2 wrapped by scripts/atf/wrap_bl2_preloader.py).
#                        Without it the HW-proven preloader digests stay compiled in.
#   URSUS_VANILLA_FIP    the one Vanilla OpenWrt U-Boot FIP this UrsusBoot may install
#                        (scripts/vanilla/make_vanilla_fip.py). Without it no Vanilla
#                        FIP is pinned and the Vanilla replacement is refused.
#   URSUS_FIP_DONOR      override the profile's donor/reference FIP
#                        (URSUS_FIP_TEMPLATE / URSUS_FIP are compatibility aliases).
#   JOBS                 parallel make jobs.
#   SOURCE_DATE_EPOCH    defaults to the commit time of HEAD for reproducible output.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REGISTRY="$ROOT/config/board-profiles.json"
RESOLVE="$ROOT/scripts/resolve_board_profile.py"
BOARD="${1:-xg040-md}"
ROLE_ARG="${2:-}"
SDK="${OPENWRT_SDK:-${3:-}}"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"

role_args=()
[ -n "$ROLE_ARG" ] && role_args=(--role "$ROLE_ARG")
PROFILE_JSON="$(python3 "$RESOLVE" --registry "$REGISTRY" --profile "$BOARD" ${role_args[@]+"${role_args[@]}"} --json)"
jget() {  # jget <key> [subkey]: print a profile value ('' for null/missing; lists one per line)
    python3 - "$PROFILE_JSON" "$@" <<'PY'
import json, sys
v = json.loads(sys.argv[1])
for k in sys.argv[2:]:
    v = v.get(k) if isinstance(v, dict) else None
if v is None:
    pass
elif isinstance(v, list):
    print("\n".join(str(x) for x in v))
elif isinstance(v, dict):
    print(json.dumps(v))
else:
    print(v)
PY
}
ROLE="$(jget runtime_role)"
CONFIG_NAME="$(jget config)"
TEMPLATE_NAME="$(jget boot_area_template)"
POLICY_HEADER="$(jget board_policy_header)"
PACKAGING="$(jget packaging)"
PIN_BASE="$(jget preloader_pin_base)"
[ -n "$CONFIG_NAME" ] || { echo "board $BOARD: profile declares no config yet" >&2; exit 3; }
CONFIG="$ROOT/config/$CONFIG_NAME"
[ -f "$CONFIG" ] || { echo "board $BOARD: missing config $CONFIG" >&2; exit 3; }
[ -f "$ROOT/boards/$POLICY_HEADER" ] || { echo "board $BOARD: missing board policy $POLICY_HEADER" >&2; exit 3; }
: "${SDK:?Set OPENWRT_SDK to an extracted official OpenWrt SDK/toolchain}"
[ -d "$SDK" ] || { echo "OpenWrt SDK/toolchain directory not found: $SDK" >&2; exit 3; }
if [ -n "${URSUS_UBI_PRELOADER:-}" ]; then
    [ -f "$URSUS_UBI_PRELOADER" ] || { echo "URSUS_UBI_PRELOADER not found: $URSUS_UBI_PRELOADER" >&2; exit 3; }
    [ -n "$PIN_BASE" ] || { echo "board $BOARD: profile has no preloader_pin_base" >&2; exit 3; }
    URSUS_UBI_PRELOADER="$(cd "$(dirname "$URSUS_UBI_PRELOADER")" && pwd)/$(basename "$URSUS_UBI_PRELOADER")"
fi
if [ -n "${URSUS_VANILLA_FIP:-}" ]; then
    [ -f "$URSUS_VANILLA_FIP" ] || { echo "URSUS_VANILLA_FIP not found: $URSUS_VANILLA_FIP" >&2; exit 3; }
    URSUS_VANILLA_FIP="$(cd "$(dirname "$URSUS_VANILLA_FIP")" && pwd)/$(basename "$URSUS_VANILLA_FIP")"
fi
grep -Fq "#define URSUS_VERSION \"$VERSION\"" "$ROOT/src/u-boot/include/ursus_version.h" || {
    echo "VERSION ($VERSION) and src/u-boot/include/ursus_version.h disagree" >&2; exit 3; }

# --- toolchain -------------------------------------------------------------
if [ -d "$SDK/staging_dir" ]; then
    export STAGING_DIR="$SDK/staging_dir"
    SEARCH_ROOT="$STAGING_DIR"
else
    SEARCH_ROOT="$SDK"
    export STAGING_DIR="$SDK"
fi
CROSS="$(find "$SEARCH_ROOT" \( -type f -o -type l \) \
    \( -name 'aarch64-openwrt-linux-musl-gcc' -o -name 'aarch64-openwrt-linux-gcc' \) | head -1 || true)"
[ -n "$CROSS" ] || { echo "AArch64 OpenWrt compiler not found under $SDK" >&2; exit 3; }
export CROSS_COMPILE="${CROSS%gcc}"
HOST_DIR=""
if [ -d "$SDK/staging_dir/host" ]; then
    HOST_DIR="$SDK/staging_dir/host"  # SDK and full buildroot (which also has hostpkg/)
elif [ -d "$SDK/staging_dir" ]; then
    HOST_DIR="$(find "$SDK/staging_dir" -maxdepth 1 -type d -name 'host*' | head -1 || true)"
fi
if [ -n "$HOST_DIR" ] && [ -d "$HOST_DIR/bin" ]; then
    export STAGING_DIR_HOST="$HOST_DIR"
    export PATH="${CROSS%/*}:$HOST_DIR/bin:$PATH"
    [ -d "$HOST_DIR/share/bison" ] && export BISON_PKGDATADIR="$HOST_DIR/share/bison"
else
    export PATH="${CROSS%/*}:$PATH"
fi
# U-Boot's default-environment generator needs xxd; the SDK ships xxdi.pl.
if ! command -v xxd >/dev/null; then
    [ -f "$SDK/scripts/xxdi.pl" ] || { echo "xxd not found and $SDK/scripts/xxdi.pl missing" >&2; exit 3; }
    SHIM="$ROOT/work/.hostshim"
    mkdir -p "$SHIM"
    printf '#!/bin/sh\nexec perl "%s/scripts/xxdi.pl" "$@"\n' "$SDK" > "$SHIM/xxd"
    chmod +x "$SHIM/xxd"
    export PATH="$SHIM:$PATH"
fi
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" show -s --format=%ct HEAD 2>/dev/null || date +%s)}"
BUILD_COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"

# --- clean work copy of the source tree ------------------------------------
WORK="$ROOT/work/$BOARD-$ROLE"
TREE="$WORK/u-boot"
rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$ROOT/src/u-boot" "$TREE"
make -s -C "$TREE" mrproper >/dev/null
rm -rf "$TREE/.binman_stamp" "$TREE/arch/arm/include/asm/arch"
printf '%s\n' "-UrsusBoot-$VERSION" > "$TREE/.scmversion"  # .scmversion is git-ignored upstream

# --- source transforms (board derivation) ----------------------------------
while IFS= read -r transform; do
    [ -n "$transform" ] || continue
    case "$transform" in
        mf-runtime)
            # MD-lineage tree -> Nokia XG-040G-MF persistent runtime (HW-cycled chain).
            cp -a "$TREE" "$WORK/u-boot-pristine"
            python3 "$ROOT/scripts/mf/mf2_ramreadonly_transform.py" "$TREE"
            python3 "$ROOT/scripts/mf/mf_runtime_enable.py" "$TREE" "$WORK/u-boot-pristine"
            python3 "$ROOT/scripts/mf/mf_hwtest8_apply.py" "$TREE"
            ;;
        *) echo "board $BOARD: unknown source transform $transform" >&2; exit 4 ;;
    esac
done < <(jget source_transforms)

python3 "$ROOT/scripts/apply_runtime_role.py" "$TREE" --role "$ROLE"
ENV_OVERLAY="$(jget env_overlay "$ROLE")"
if [ -n "$ENV_OVERLAY" ]; then
    ENV_SRC="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["src"])' "$ENV_OVERLAY")"
    ENV_DST="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["dst"])' "$ENV_OVERLAY")"
    cp "$ROOT/$ENV_SRC" "$TREE/$ENV_DST"
fi
python3 "$ROOT/scripts/apply_board_policy.py" "$TREE" "$ROOT/boards/$POLICY_HEADER"
# Identity comes from VERSION only (transforms such as mf-runtime carry their own).
printf '%s\n' "-UrsusBoot-$VERSION" > "$TREE/.scmversion"
sed -E -i "s/^#define URSUS_VERSION \"[^\"]+\"/#define URSUS_VERSION \"$VERSION\"/" "$TREE/include/ursus_version.h"
grep -Fq "#define URSUS_VERSION \"$VERSION\"" "$TREE/include/ursus_version.h"
if [ -n "${URSUS_UBI_PRELOADER:-}" ]; then
    python3 "$ROOT/scripts/pin_ubi_preloader.py" --tree "$TREE" --preloader "$URSUS_UBI_PRELOADER" \
        --from "$PIN_BASE" | tee "$WORK/ubi-preloader-pin.txt"
fi
if [ -n "${URSUS_VANILLA_FIP:-}" ]; then
    python3 "$ROOT/scripts/pin_vanilla_fip.py" --tree "$TREE" --fip "$URSUS_VANILLA_FIP" | tee "$WORK/vanilla-fip-pin.txt"
fi

# --- configuration ----------------------------------------------------------
mapfile -t FRAGMENTS < <(python3 "$RESOLVE" --registry "$REGISTRY" --profile "$BOARD" --role "$ROLE" --config-dir "$ROOT/config")
while IFS= read -r extra; do
    [ -n "$extra" ] && FRAGMENTS+=("$ROOT/config/$extra")
done < <(jget role_fragments "$ROLE")
cp "$CONFIG" "$TREE/.config"
python3 "$ROOT/scripts/apply_kconfig_fragment.py" --config "$TREE/.config" "${FRAGMENTS[@]}"
make -C "$TREE" olddefconfig
python3 "$ROOT/scripts/apply_kconfig_fragment.py" --check-only --config "$TREE/.config" "${FRAGMENTS[@]}"
while IFS= read -r sym; do
    [ -n "$sym" ] || continue
    grep -q "^${sym}=y" "$TREE/.config" || { echo "board $BOARD: required ${sym} is not enabled" >&2; exit 4; }
done < <(jget config_require)
while IFS= read -r sym; do
    [ -n "$sym" ] || continue
    ! grep -q "^${sym}=y" "$TREE/.config" || { echo "board $BOARD: forbidden ${sym}=y" >&2; exit 4; }
done < <(jget config_forbid)

# --- compile ----------------------------------------------------------------
make -C "$TREE" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
OUT="$ROOT/dist/$BOARD"
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$TREE/u-boot.bin" "$OUT/u-boot.bin"
cp "$TREE/.config" "$OUT/u-boot.config"

python3 - "$OUT/u-boot.bin" "$VERSION" "$PROFILE_JSON" "${URSUS_UBI_PRELOADER:-}" "$PIN_BASE" "${URSUS_VANILLA_FIP:-}" <<'PY'
import hashlib, json, sys
raw = open(sys.argv[1], "rb").read()
version, profile, preloader, base = sys.argv[2], json.loads(sys.argv[3]), sys.argv[4], sys.argv[5]
vanilla = sys.argv[6]
if vanilla:
    vd = hashlib.sha256(open(vanilla, "rb").read()).hexdigest()
    # The compiled-in byte array is what ursus_vanilla_fip_validate() memcmp()s against.
    if bytes.fromhex(vd) not in raw:
        raise SystemExit(f"Vanilla FIP digest {vd} is not compiled into u-boot.bin")
    print(f"VANILLA_FIP_ACCEPTS={vd}")
if b"REPLACE-URSUSBOOT-WITH-VANILLA" not in raw:
    raise SystemExit("Vanilla replacement backend missing from u-boot.bin")
must = [version] + profile.get("binary_require", [])
for m in must:
    if m.encode() not in raw:
        raise SystemExit(f"binary marker missing: {m}")
for m in profile.get("binary_forbid", []):
    if m.encode() in raw:
        raise SystemExit(f"forbidden binary marker present: {m}")
proven = {
    "md": ("6c3b2339d036340396730a13adfe35c0d2a4dddedeffb6f9965a24e0c7908808",
           "6f9c928bad500de0339bbfdfa354c17a7ac044f96c913f3a01301971d6cd659d"),
    "mf": ("778d10a65276085b70bec005248fc87ec208b43b0239502f15ade20fe528301e",
           "c655479c4d14b4f6d1a7a5eb8de80bfb204b3cdf46c9e48a5c6f3aea22d98131"),
}
if base:
    if preloader:
        pre = open(preloader, "rb").read()
        cand = b"\xff" * 0x800 + pre
        cand += b"\xff" * (0x20000 - len(cand))
        want = (hashlib.sha256(pre).hexdigest(), hashlib.sha256(cand).hexdigest())
        for old in proven[base]:
            if bytes.fromhex(old) in raw or old.encode() in raw:
                raise SystemExit(f"old UBI preloader digest {old} survived the pin")
    else:
        want = proven[base]
    for d in want:
        # The compiled-in byte arrays are what UrsusBoot memcmp()s against.
        if bytes.fromhex(d) not in raw:
            raise SystemExit(f"UBI preloader digest {d} is not compiled into u-boot.bin")
    print(f"UBI_PRELOADER_ACCEPTS={want[0]} BL2_IMAGE={want[1]}")
print(f"BINARY_CONTRACT=PASS version={version} bytes={len(raw)}")
PY

# --- packaging --------------------------------------------------------------
REF="${URSUS_FIP_DONOR:-${URSUS_FIP_TEMPLATE:-${URSUS_FIP:-}}}"
if [ -z "$REF" ]; then
    REF_NAME="$(jget reference_fip)"
    [ -n "$REF_NAME" ] && REF="$ROOT/$REF_NAME"
fi
sum_files=("$OUT/u-boot.bin")
case "$PACKAGING" in
persistent-fip)
    # Donor FIP semantics: this container supplies the proven platform/FIP lineage.
    # Its NT_FW/BL33 payload is always replaced by the U-Boot built above.
    [ -n "$REF" ] && [ -f "$REF" ] || { echo "board $BOARD: donor FIP not found: ${REF:-<none>}" >&2; exit 5; }
    TEMPLATE="$ROOT/$TEMPLATE_NAME"
    [ -f "$TEMPLATE" ] || { echo "board $BOARD: missing boot-area template $TEMPLATE" >&2; exit 5; }
    HOSTCC="${HOSTCC:-gcc}"
    command -v "$HOSTCC" >/dev/null || { echo "host C compiler not found: $HOSTCC" >&2; exit 5; }
    HOST_LZMA="$WORK/lzma1ext_noeopm"
    "$HOSTCC" -O2 -Wall -Wextra "$ROOT/src/u-boot/lzma1ext_noeopm.c" -llzma -o "$HOST_LZMA" || {
        echo 'cannot build the host LZMA1EXT packer (liblzma development headers missing?).' >&2
        exit 5
    }
    "$HOST_LZMA" "$OUT/u-boot.bin" "$OUT/u-boot.lzma" 1048576
    FIP_OUT="$OUT/ursusboot-update.fip"
    python3 "$ROOT/src/u-boot/repack_persistent_fip.py" "$REF" "$OUT/u-boot.lzma" "$FIP_OUT"
    python3 - "$FIP_OUT" "$OUT/u-boot.lzma" <<'PY'
import hashlib, struct, sys
from pathlib import Path
fip=Path(sys.argv[1]).read_bytes(); want=Path(sys.argv[2]).read_bytes()
nt_uuid=bytes.fromhex('d6d0eea7fcead54b97829934f234b6e4')
if struct.unpack_from('<I',fip,0)[0] != 0xaa640001: raise SystemExit('generated FIP magic mismatch')
pos=16; found=None; nt=None
while pos+40 <= len(fip):
    uid=fip[pos:pos+16]
    if uid == b'\0'*16: break
    off,size,_=struct.unpack_from('<QQQ',fip,pos+16)
    if uid == nt_uuid: found=fip[off:off+size]; nt=(off,size); break
    pos += 40
if found is None: raise SystemExit('generated FIP has no NT_FW/BL33 entry')
if found != want: raise SystemExit('generated FIP BL33 does not match current u-boot.lzma')
if len(fip) >= 0x7b800: raise SystemExit(f'generated FIP exceeds stock budget: {len(fip):#x}')
# Stock boot-area contract: BL33 must end before the certificate block at 0x77800.
if nt[0] + nt[1] > 0x77800: raise SystemExit(f'NT_FW overflows into certificates: {nt}')
print('FIP_CURRENT_BL33=PASS')
print(f'NT_FW_OFF=0x{nt[0]:x} NT_FW_SIZE={nt[1]} NT_FW_MARGIN={0x77800-(nt[0]+nt[1])}')
print('FIP_SHA256='+hashlib.sha256(fip).hexdigest())
print('BL33_LZMA_SHA256='+hashlib.sha256(want).hexdigest())
PY
    python3 "$ROOT/scripts/make-install-mtd0.py" --template "$TEMPLATE" --fip "$FIP_OUT" --output "$OUT/ursusboot-install-mtd0.bin"
    sum_files+=("$OUT/u-boot.lzma" "$FIP_OUT" "$OUT/ursusboot-install-mtd0.bin")
    ;;
mf-runtime)
    # MF persistent runtime: BL33 for the device-derived UBI/raw FIP install
    # (u-boot.runtime.lzma) plus the UART/BootROM RAM-recovery pair.
    [ -n "$REF" ] && [ -f "$REF" ] || { echo "board $BOARD: MF donor FIP not found: ${REF:-<none>}" >&2; exit 5; }
    UART_PRELOADER="$ROOT/$(jget uart_preloader)"
    [ -f "$UART_PRELOADER" ] || { echo "board $BOARD: UART preloader not found: $UART_PRELOADER" >&2; exit 5; }
    python3 "$ROOT/scripts/mf/mf2_repack_from_medve.py" \
        --medve-patcher "$ROOT/scripts/mf/medve/patch_recovery_safe_fip.py" \
        --source "$REF" \
        --bl33-raw "$OUT/u-boot.bin" \
        --bl33-output "$OUT/u-boot.runtime.lzma" \
        --output "$OUT/ursusboot-runtime-ram.fip" \
        --report "$OUT/MF-RUNTIME-FIP-REPACK.json"
    python3 - "$OUT" <<'PY'
import json, lzma, sys
from pathlib import Path
out = Path(sys.argv[1])
r = json.loads((out / "MF-RUNTIME-FIP-REPACK.json").read_text(encoding="ascii"))
for k in ("bl31_byte_exact", "mf2_bl33_roundtrip", "mf2_bl33_lzma_known_size",
          "serial_preserved", "flags_preserved", "uuid_flags_preserved"):
    if r.get(k) is not True:
        raise SystemExit(f"MF runtime FIP QA failed: {k}={r.get(k)}")
if r.get("entry_count") != 2 or r.get("mf2_bl33_lzma_eopm") is not False:
    raise SystemExit("MF runtime FIP QA failed: entry_count/eopm")
raw = lzma.decompress((out / "u-boot.runtime.lzma").read_bytes(), format=lzma.FORMAT_ALONE)
if raw != (out / "u-boot.bin").read_bytes():
    raise SystemExit("u-boot.runtime.lzma does not decompress to u-boot.bin")
print("MF_RUNTIME_FIP_QA=PASS sha256=" + r["output_sha256"])
PY
    cp "$UART_PRELOADER" "$OUT/ursusboot-uart-preloader.bin"
    sum_files+=("$OUT/u-boot.runtime.lzma" "$OUT/ursusboot-runtime-ram.fip" "$OUT/ursusboot-uart-preloader.bin")
    ;;
"")
    echo "board $BOARD: no packaging declared; raw u-boot.bin only" >&2
    ;;
*)
    echo "board $BOARD: unknown packaging $PACKAGING" >&2; exit 5 ;;
esac

{
    echo "UrsusBoot $VERSION"
    echo "BOARD=$BOARD"
    echo "MODEL=$(jget vendor) $(jget model)"
    echo "SOC=$(jget soc)"
    echo "ROLE=$ROLE"
    echo "PACKAGING=${PACKAGING:-none}"
    echo "BUILD_COMMIT=$BUILD_COMMIT"
    echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    [ -n "$REF" ] && echo "DONOR_FIP_SHA256=$(sha256sum "$REF" | cut -d' ' -f1)"
    if [ -n "${URSUS_UBI_PRELOADER:-}" ]; then
        grep '^UBI_' "$WORK/ubi-preloader-pin.txt"
    else
        echo "UBI_PRELOADER=HW_PROVEN_DEFAULT"
    fi
    if [ -n "${URSUS_VANILLA_FIP:-}" ]; then
        grep '^VANILLA_' "$WORK/vanilla-fip-pin.txt"
    else
        echo "VANILLA_FIP=NOT_PINNED"
    fi
} > "$OUT/BUILD-INFO.txt"
if [ -n "${URSUS_VANILLA_FIP:-}" ]; then
    cp "$URSUS_VANILLA_FIP" "$OUT/vanilla-u-boot.fip"
    sum_files+=("$OUT/vanilla-u-boot.fip")
fi
sha256sum "${sum_files[@]}" | tee "$OUT/SHA256SUMS"
echo "URSUSBOOT_BUILD=OK board=$BOARD role=$ROLE version=$VERSION out=dist/$BOARD"

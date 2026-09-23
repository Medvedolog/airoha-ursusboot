#!/usr/bin/env bash
# QA of the modular build pipeline (TEST63+). Needs no cross toolchain:
# profile schema, pinned reference inputs, preloader wrap/pin selftests and a
# dry run of the MD/MF source transforms + board policies on a scratch copy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 -m py_compile "$ROOT"/scripts/mf/*.py "$ROOT"/scripts/mf/medve/*.py "$ROOT"/scripts/atf/*.py \
    "$ROOT"/scripts/vanilla/*.py "$ROOT"/scripts/pin_vanilla_fip.py
bash -n "$ROOT/build.sh" "$ROOT/scripts/ci/build-release.sh"
grep -q '^/work/$' "$ROOT/.gitignore"
V="$(tr -d '[:space:]' < "$ROOT/VERSION")"
grep -Fq "#define URSUS_VERSION \"$V\"" "$ROOT/src/u-boot/include/ursus_version.h"

python3 "$ROOT/scripts/qa_pipeline_checks.py" profiles "$ROOT"

QA_TMP="$(mktemp -d)"
trap 'rm -rf "$QA_TMP"' EXIT
python3 "$ROOT/scripts/qa_pipeline_checks.py" pin "$ROOT" "$QA_TMP"
python3 "$ROOT/scripts/qa_pipeline_checks.py" vanilla "$ROOT" "$QA_TMP"

mkdir -p "$QA_TMP/tree"
cp -a "$ROOT/src/u-boot" "$QA_TMP/tree/md"
make -s -C "$QA_TMP/tree/md" mrproper >/dev/null 2>&1 || true
printf '%s\n' "-UrsusBoot-$V" > "$QA_TMP/tree/md/.scmversion"
cp -a "$QA_TMP/tree/md" "$QA_TMP/tree/mf"
cp -a "$QA_TMP/tree/md" "$QA_TMP/tree/mf-pristine"
python3 "$ROOT/scripts/apply_board_policy.py" "$QA_TMP/tree/md" "$ROOT/boards/xg040-md.h" >/dev/null
python3 "$ROOT/scripts/mf/mf2_ramreadonly_transform.py" "$QA_TMP/tree/mf" >/dev/null
python3 "$ROOT/scripts/mf/mf_runtime_enable.py" "$QA_TMP/tree/mf" "$QA_TMP/tree/mf-pristine" >/dev/null
python3 "$ROOT/scripts/mf/mf_hwtest8_apply.py" "$QA_TMP/tree/mf" >/dev/null
python3 "$ROOT/scripts/apply_board_policy.py" "$QA_TMP/tree/mf" "$ROOT/boards/xg040-mf.h" >/dev/null
# Shared lwIP/WebFailsafe fixes must survive the MF derivation.
grep -q 'URSUS_CONSOLE_DEFER' "$QA_TMP/tree/mf/cmd/ursusweb.c"
grep -q 'bool borrowed = false' "$QA_TMP/tree/mf/net/lwip/tftp.c"
grep -q 'bool borrowed = false' "$QA_TMP/tree/mf/cmd/lwip/ping.c"
python3 "$ROOT/scripts/qa_pipeline_checks.py" transforms "$ROOT" "$QA_TMP/tree"
echo URSUSBOOT_PIPELINE_QA=PASS

#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 -m py_compile "$ROOT"/scripts/*.py
ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os, json, hashlib
r=Path(os.environ['ROOT'])
checks={'boards/nokia-xg040-md/stock-mtd0-template.bin':'0256f49128aa00e2b5c2d613c1ae9b06d2278010','boards/nokia-xg040-mf/stock-mtd0-template.bin':'54250bac7bb00b0938b7702df2ea522c4464a0cd'}
for rel,want in checks.items():
 b=(r/rel).read_bytes(); assert len(b)==0x80000,(rel,len(b)); got=hashlib.sha1(f'blob {len(b)}\0'.encode()+b).hexdigest(); assert got==want,(rel,got)
f=(r/'reference/md/ursusboot-test61-update.fip').read_bytes(); assert len(f)==503808 and f[:4]==b'\x01\x00\x64\xaa'
reg=json.loads((r/'config/board-profiles.json').read_text())
for name,p in reg['profiles'].items():
 for field,base in (('config','config'),('boot_area_template',''),('reference_fip','')):
  if field not in p: raise SystemExit(f'profile {name}: missing {field}')
  v=p[field]
  if v is not None:
   path=r/base/v if base else r/v
   if not path.is_file(): raise SystemExit(f'profile {name}: {field} missing {path}')
 role=p['default_role']
 if role not in p['allowed_roles'] or role not in reg['runtime_roles']: raise SystemExit(f'profile {name}: bad default role')
print('board templates + profile registry: PASS')
PY
for p in Makefile Kconfig arch board common drivers include net; do test -e "$ROOT/src/u-boot/$p" || exit 1; done
grep -q 'Repository policy: self-contained' "$ROOT/README.md"
for helper in resolve_board_profile.py apply_runtime_role.py make-install-mtd0.py repack_persistent_fip.py lzma1ext_noeopm.c; do grep -q "$helper" "$ROOT/build.sh" || { echo "build.sh missing $helper" >&2; exit 1; }; done
test -f "$ROOT/src/u-boot/repack_persistent_fip.py"
test -f "$ROOT/src/u-boot/lzma1ext_noeopm.c"
grep -q 'FIP_CURRENT_BL33=PASS' "$ROOT/build.sh"
grep -q 'URSUS_FIP_DONOR' "$ROOT/README.md"
grep -q 'Donor FIP semantics' "$ROOT/README.md"
grep -q 'treated as a \*\*donor\*\*' "$ROOT/README.md"

# Repacking the reference FIP with its own NT_FW payload must reproduce the
# container byte-for-byte. This guards the donor/repack contract independently
# of the cross-toolchain.
ROOT="$ROOT" python3 - <<'PY'
import importlib.util, os
from pathlib import Path
r = Path(os.environ['ROOT'])
spec = importlib.util.spec_from_file_location('repack', r / 'src/u-boot/repack_persistent_fip.py')
repack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repack)
ref = (r / 'reference/md/ursusboot-test61-update.fip').read_bytes()
_s, _f, entries, _t, _e = repack.parse(ref)
nt = next(e['payload'] for e in entries if e['uuid'] == repack.NT_UUID)
out, _report = repack.rebuild(ref, nt, force_old_layout=True)
if out != ref:
    raise SystemExit('FIP repack selftest failed: rebuilt container differs from reference')
print('FIP repack selftest: PASS')
PY
if grep -R -nE 'vendor-baseline|git clone .*airoha-router-ursusflasher|raw\.githubusercontent\.com/Medvedolog/airoha-router-ursusflasher|codeload\.github\.com/Medvedolog/airoha-router-ursusflasher' "$ROOT" --exclude='PROVENANCE.md' --exclude='qa.sh'; then echo 'unexpected build/runtime dependency on old repository' >&2; exit 1; fi
grep -q 'Network busy: active lwIP interface' "$ROOT/src/u-boot/net/lwip/net-lwip.c"
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/cmd/lwip/ping.c"
grep -A4 -q 'ret = ping_raw_init(&ctx);' "$ROOT/src/u-boot/cmd/lwip/ping.c"
grep -A4 'ret = ping_raw_init(&ctx);' "$ROOT/src/u-boot/cmd/lwip/ping.c" | grep -q 'if (!borrowed)'
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/net/lwip/tftp.c"
grep -q 'URSUS_CONSOLE_DEFER' "$ROOT/src/u-boot/cmd/ursusweb.c"
grep -q 'URSUS_WEB_STOP_REQUEST source=UART' "$ROOT/src/u-boot/cmd/ursusweb.c"
# MAC identity must be refreshed before autoboot on both current Nokia profiles.
grep -q '^CONFIG_USE_PREBOOT=y$' "$ROOT/config/u-boot.TEST61.full.config"
grep -q '^CONFIG_USE_PREBOOT=y$' "$ROOT/config/an7583_nokia_xg-040g-mf_MF2_RAM_defconfig"
# Without ENV_OVERWRITE the ethaddr env flag is "mo" (write-once), so every
# ethaddr_factory rewrite after the first saveenv is refused and the recovery
# MAC can never be corrected. See include/env_flags.h ETHADDR_FLAGS.
grep -q '^CONFIG_ENV_OVERWRITE=y$' "$ROOT/config/u-boot.TEST61.full.config"
grep -q '^CONFIG_ENV_OVERWRITE=y$' "$ROOT/config/an7583_nokia_xg-040g-mf_MF2_RAM_defconfig"
for envf in "$ROOT/src/u-boot/defenvs/an7581_nokia_xg-040g-md_env" "$ROOT/src/u-boot/defenvs/an7583_nokia_xg-040g-mf_env"; do
    grep -q '^preboot=run ethaddr_factory ;' "$envf"
    grep -q 'URSUS_MAC_SOURCE=RI' "$envf"
    grep -q 'URSUS_MAC_INVALID_RI fallback=persisted' "$envf"
    grep -q 'URSUS_MAC_RI_READ_FAIL fallback=random' "$envf"
    grep -q 'env default -f -a && run ethaddr_factory && saveenv && saveenv' "$envf"
done
grep -q '^## Recovery threat model$' "$ROOT/README.md"
test ! -e "$ROOT/SHA256SUMS" || { echo 'root SHA256SUMS is intentionally unsupported; use dist/<board>/SHA256SUMS' >&2; exit 1; }
if python3 "$ROOT/scripts/resolve_board_profile.py" --registry "$ROOT/config/board-profiles.json" --profile xg040-md --role bogus --field runtime_role >/tmp/qa.out 2>&1; then exit 1; fi
grep -q "does not allow runtime role 'bogus'" /tmp/qa.out
if python3 "$ROOT/scripts/resolve_board_profile.py" --registry "$ROOT/config/board-profiles.json" --profile xg140-md --field config >/tmp/qa.out 2>&1; then exit 1; fi
grep -q "declares no 'config' yet" /tmp/qa.out
echo URSUSBOOT_STANDALONE_QA=PASS

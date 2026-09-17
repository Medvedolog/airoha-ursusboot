#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 -m py_compile "$ROOT"/scripts/*.py
ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os, json, hashlib
r = Path(os.environ['ROOT'])
reg = json.loads((r / 'config/board-profiles.json').read_text())
checks = {
    'boards/nokia-xg040-md/stock-mtd0-template.bin': '0256f49128aa00e2b5c2d613c1ae9b06d2278010',
    'boards/nokia-xg040-mf/stock-mtd0-template.bin': '54250bac7bb00b0938b7702df2ea522c4464a0cd',
}
for rel, want in checks.items():
    b = (r / rel).read_bytes()
    assert len(b) == 0x80000, (rel, len(b))
    got = hashlib.sha1(f'blob {len(b)}\0'.encode() + b).hexdigest()
    assert got == want, (rel, got)
f = (r / 'reference/md/ursusboot-test61-update.fip').read_bytes()
assert len(f) == 503808 and f[:4] == b'\x01\x00\x64\xaa'
print('board templates + TEST61 FIP: PASS')

# Every path a board profile declares must exist. A profile may declare null
# (described but not yet buildable); it may not point at something missing.
for name, prof in reg['profiles'].items():
    for field, base in (('config', 'config'), ('boot_area_template', ''), ('reference_fip', '')):
        if field not in prof:
            raise SystemExit(f'profile {name}: field {field} is not declared')
        value = prof[field]
        if value is None:
            continue
        path = r / base / value if base else r / value
        if not path.is_file():
            raise SystemExit(f'profile {name}: {field} points at missing {path}')
    role = prof['default_role']
    if role not in prof['allowed_roles'] or role not in reg['runtime_roles']:
        raise SystemExit(f'profile {name}: default_role {role} is not a valid role')
print('board profile paths + roles: PASS')
PY

for p in Makefile Kconfig arch board common drivers include net; do
    test -e "$ROOT/src/u-boot/$p" || { echo "missing complete source path: src/u-boot/$p" >&2; exit 1; }
done
for p in lzma1ext_noeopm.c repack_persistent_fip.py; do
    test -f "$ROOT/src/u-boot/$p" || { echo "missing FIP packaging helper: src/u-boot/$p" >&2; exit 1; }
done

grep -q 'Repository policy: self-contained' "$ROOT/README.md"

# build.sh must stay data-driven and must package the freshly built BL33 into
# the generated FIP rather than embedding the reference TEST61 payload.
if grep -qE '^\s*(xg040-md|xg040-mf|xg140-md)\)' "$ROOT/build.sh"; then
    echo 'build.sh hardcodes board names; use config/board-profiles.json' >&2
    exit 1
fi
for helper in resolve_board_profile.py apply_runtime_role.py make-install-mtd0.py repack_persistent_fip.py lzma1ext_noeopm.c; do
    grep -q "$helper" "$ROOT/build.sh" || {
        echo "build.sh no longer uses $helper" >&2
        exit 1
    }
done
grep -q 'FIP_CURRENT_BL33=PASS' "$ROOT/build.sh" || {
    echo 'build.sh no longer verifies generated FIP BL33 against current build' >&2
    exit 1
}

# Resolver negative paths are part of the public contract.
if python3 "$ROOT/scripts/resolve_board_profile.py" --registry "$ROOT/config/board-profiles.json" --profile xg040-md --role bogus --field runtime_role >/tmp/ursus-role.out 2>&1; then
    echo 'resolver unexpectedly accepted bogus runtime role' >&2
    exit 1
fi
grep -q "does not allow runtime role 'bogus'" /tmp/ursus-role.out
if python3 "$ROOT/scripts/resolve_board_profile.py" --registry "$ROOT/config/board-profiles.json" --profile xg140-md --field config >/tmp/ursus-profile.out 2>&1; then
    echo 'resolver unexpectedly treated xg140-md as buildable' >&2
    exit 1
fi
grep -q "declares no 'config' yet" /tmp/ursus-profile.out
rm -f /tmp/ursus-role.out /tmp/ursus-profile.out

if grep -R -nE 'vendor-baseline|git clone .*airoha-router-ursusflasher|raw\.githubusercontent\.com/Medvedolog/airoha-router-ursusflasher|codeload\.github\.com/Medvedolog/airoha-router-ursusflasher' "$ROOT" --exclude='PROVENANCE.md' --exclude='qa.sh'; then
    echo 'unexpected build/runtime dependency on the old repository' >&2
    exit 1
fi

# Shared lwIP ownership contract: WebFailsafe keeps the live netif; ping/TFTP
# borrow it, Web Console commands are deferred out of TCP callbacks, and UART
# Ctrl-C provides the explicit escape back to the normal U-Boot shell.
grep -q 'Network busy: active lwIP interface' "$ROOT/src/u-boot/net/lwip/net-lwip.c"
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/cmd/lwip/ping.c"
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/net/lwip/tftp.c"
grep -q 'URSUS_CONSOLE_DEFER' "$ROOT/src/u-boot/cmd/ursusweb.c"
grep -q 'URSUS_WEB_STOP_REQUEST source=UART' "$ROOT/src/u-boot/cmd/ursusweb.c"
grep -q 'URSUS_WEB_STOPPED restart=ursusweb' "$ROOT/src/u-boot/cmd/ursusweb.c"

echo URSUSBOOT_STANDALONE_QA=PASS

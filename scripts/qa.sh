#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 -m py_compile "$ROOT"/scripts/*.py
ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os, json, hashlib
r = Path(os.environ['ROOT'])
json.loads((r / 'config/board-profiles.json').read_text())
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
PY
for p in Makefile Kconfig arch board common drivers include net; do
    test -e "$ROOT/src/u-boot/$p" || { echo "missing complete source path: src/u-boot/$p" >&2; exit 1; }
done
grep -q 'Repository policy: self-contained' "$ROOT/README.md"
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

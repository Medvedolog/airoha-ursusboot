#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; python3 -m py_compile "$ROOT"/scripts/*.py
ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os,json,hashlib
r=Path(os.environ['ROOT']); json.loads((r/'config/board-profiles.json').read_text())
for rel,want in {'boards/nokia-xg040-md/stock-mtd0-template.bin':'0256f49128aa00e2b5c2d613c1ae9b06d2278010','boards/nokia-xg040-mf/stock-mtd0-template.bin':'54250bac7bb00b0938b7702df2ea522c4464a0cd'}.items():
    b=(r/rel).read_bytes(); assert len(b)==0x80000; got=hashlib.sha1(f'blob {len(b)}\0'.encode()+b).hexdigest(); assert got==want,(rel,got)
f=(r/'reference/md/ursusboot-test61-update.fip').read_bytes(); assert len(f)==503808 and f[:4]==b'\x01\x00\x64\xaa'
print('board templates + TEST61 FIP: PASS')
PY
test -d "$ROOT/src/u-boot"; grep -q 'Repository policy: self-contained' "$ROOT/README.md"; echo URSUSBOOT_STANDALONE_QA=PASS

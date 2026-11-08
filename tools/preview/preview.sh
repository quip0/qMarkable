#!/bin/bash
# Render the homepage offscreen with sample data (no tablet needed).
#
# One-time setup (see README "Developing"):
#   WORK=~/qmarkable-work  (holds the extracted stock QML + hashtab, never committed)
#   python3 -m venv $WORK/venv && $WORK/venv/bin/pip install zstandard pyside6-essentials
#   cargo build --release in a clone of https://github.com/asivery/qmldiff
#
# Usage: WORK=... QMLDIFF=.../qmldiff tools/preview/preview.sh
set -euo pipefail
: "${WORK:?set WORK}"; : "${QMLDIFF:?set QMLDIFF to the qmldiff binary}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PY="$WORK/venv/bin/python"
OUT="$WORK/preview"; mkdir -p "$OUT"
rm -rf "$WORK/applied"
"$QMLDIFF" apply-diffs --hashtab "$WORK/hashtab" "$WORK/qml" "$WORK/applied" "$REPO"/mods/*.qmd | grep -v "^Reading"
cd "$OUT"
[ -d data ] || "$PY" -I "$REPO/tools/preview/gen.py" "$OUT/data"
"$PY" -I - "$WORK/applied/qml/device/view/main/MainView.qml" <<'PYEOF'
import sys
lines=open(sys.argv[1]).read().split('\n')
def block(idname):
    i=[n for n,l in enumerate(lines) if l.strip()==f'id: {idname}'][0]
    start=i-1
    while not lines[start].rstrip().endswith('{'): start-=1
    depth=0; out=[]
    for l in lines[start:]:
        out.append(l); depth+=l.count('{')-l.count('}')
        if depth==0: break
    return '\n'.join(out)
open('blocks.qml.part','w').write(block('qmHome')+'\n'+block('qmDocMarker'))
PYEOF
"$PY" -I "$REPO/tools/preview/build.py" "$OUT/data" 0 Main.qml
"$PY" -I "$REPO/tools/preview/build.py" "$OUT/data" 1 MainOpen.qml
"$WORK/venv/bin/pyside6-qmllint" Main.qml 2>&1 | grep -E "^(Warning|Error)" | grep -v -i unqualified || true
export QT_QPA_PLATFORM=offscreen QML_XHR_ALLOW_FILE_READ=1 QML_XHR_ALLOW_FILE_WRITE=1
"$PY" -I "$REPO/tools/preview/shot.py" Main.qml home.png 2>&1 | grep -v qt.qpa.fonts || true
"$PY" -I "$REPO/tools/preview/shot.py" MainOpen.qml home-detail.png 2>&1 | grep -v qt.qpa.fonts || true
echo "Wrote $OUT/home.png and $OUT/home-detail.png"

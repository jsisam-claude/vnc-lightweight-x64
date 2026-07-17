#!/usr/bin/env bash
#
# ci_linux.sh — the in-container verification gate (also run by CI).
# Builds under ASan/UBSan and exercises the portable, security-critical paths:
# unit tests, the cross-process encoding matrix, and the audio parser. Any
# sanitizer finding or checksum mismatch fails the run.
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD=build/linux-asan
echo "== configure + build ($BUILD) =="
cmake --preset linux-asan >/dev/null
cmake --build "$BUILD"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"

echo "== unit tests =="
"$BUILD/audio_test"
"$BUILD/ftpath_test"

echo "== start a VNC server for the matrix =="
PASS_DIR="$(mktemp -d)"
trap 'kill "${XVNC_PID:-0}" 2>/dev/null || true; rm -rf "$PASS_DIR"' EXIT
printf 'ci-secret\nci-secret\n' | vncpasswd -f > "$PASS_DIR/passwd" 2>/dev/null || \
  { echo "vncpasswd unavailable"; exit 1; }
chmod 600 "$PASS_DIR/passwd"
Xvnc :19 -geometry 800x600 -depth 24 -SecurityTypes VncAuth \
  -rfbauth "$PASS_DIR/passwd" -localhost >/tmp/ci_xvnc.log 2>&1 &
XVNC_PID=$!
sleep 3

echo "== cross-process encoding matrix (worker + IPC + shm vs direct client) =="
export VNC_PASSWORD=ci-secret
REF=""
FAIL=0
for enc in raw copyrect rre corre hextile zlib trle zrle ultra; do
  out="$("$BUILD/ipc_test" 127.0.0.1:5919 --encodings "$enc" --updates 3 \
        --worker "$BUILD/vncworker" 2>/tmp/ci_$enc || true)"
  cs="$(echo "$out" | grep -oE 'checksum=[0-9a-f]+' || true)"
  [ -z "$REF" ] && REF="$cs"
  if [ "$cs" != "$REF" ] || [ -z "$cs" ]; then
    echo "  $enc: MISMATCH/EMPTY ($cs vs $REF)"; FAIL=1
  elif grep -qE "ERROR: AddressSanitizer|runtime error:|LeakSanitizer" /tmp/ci_$enc; then
    echo "  $enc: SANITIZER FINDING"; FAIL=1
  else
    echo "  $enc: ok ($cs)"
  fi
done

[ "$FAIL" = 0 ] && echo "== PASS ==" || { echo "== FAIL =="; exit 1; }

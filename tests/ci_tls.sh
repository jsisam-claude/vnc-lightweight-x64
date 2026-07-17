#!/usr/bin/env bash
#
# ci_tls.sh — VeNCrypt/X509 verification via the GnuTLS reference backend.
# Proves the TLS path and certificate verification wiring end-to-end against a
# real QEMU x509 VNC server: correct CA connects, an unrelated CA is rejected,
# and no CA fails closed. (The shipped SChannel backend must match this.)
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD=build/linux-tls
echo "== configure + build ($BUILD, GnuTLS) =="
cmake --preset linux-tls >/dev/null
cmake --build "$BUILD"

CERT="$(mktemp -d)"
QEMU_PID=""
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -rf "$CERT"' EXIT

echo "== generate test CA + server cert =="
cat > "$CERT/ca.cfg" <<'EOF'
cn = "VNC CI CA"
ca
cert_signing_key
EOF
cat > "$CERT/server.cfg" <<'EOF'
cn = "127.0.0.1"
ip_address = "127.0.0.1"
tls_www_server
encryption_key
signing_key
EOF
certtool --generate-privkey --outfile "$CERT/ca-key.pem" >/dev/null 2>&1
certtool --generate-self-signed --load-privkey "$CERT/ca-key.pem" \
  --template "$CERT/ca.cfg" --outfile "$CERT/ca-cert.pem" >/dev/null 2>&1
certtool --generate-privkey --outfile "$CERT/server-key.pem" >/dev/null 2>&1
certtool --generate-certificate --load-privkey "$CERT/server-key.pem" \
  --load-ca-certificate "$CERT/ca-cert.pem" --load-ca-privkey "$CERT/ca-key.pem" \
  --template "$CERT/server.cfg" --outfile "$CERT/server-cert.pem" >/dev/null 2>&1
# unrelated CA (never signed the server)
certtool --generate-privkey --outfile "$CERT/other-key.pem" >/dev/null 2>&1
certtool --generate-self-signed --load-privkey "$CERT/other-key.pem" \
  --template "$CERT/ca.cfg" --outfile "$CERT/other-cert.pem" >/dev/null 2>&1

echo "== start QEMU with x509 VNC =="
qemu-system-x86_64 -display none -m 128 \
  -object "tls-creds-x509,id=tls0,dir=$CERT,endpoint=server,verify-peer=no" \
  -vnc 127.0.0.1:13,tls-creds=tls0 >/tmp/ci_qemu.log 2>&1 &
QEMU_PID=$!
sleep 3

export ASAN_OPTIONS=detect_leaks=0 # vendored GnuTLS leaks at exit (test-only backend)
# Capture output into a variable (not a pipeline): vnctest may exit non-zero even
# on a successful connect, and `set -o pipefail` would otherwise mask the grep.
run() { "$BUILD/vnctest" 127.0.0.1:5913 --frames 1 --timeout-ms 8000 "$@" 2>&1 || true; }

FAIL=0
echo "== correct CA must connect =="
if run --ca "$CERT/ca-cert.pem" | grep -q "connected:"; then echo "  ok"; else echo "  FAIL"; FAIL=1; fi
echo "== unrelated CA must be rejected =="
out="$(run --ca "$CERT/other-cert.pem")"
if echo "$out" | grep -qiE "not trusted|failed"; then echo "  ok"; else echo "  FAIL"; FAIL=1; fi
echo "== no CA must fail closed =="
out="$(run)"
if echo "$out" | grep -qiE "credential|failed"; then echo "  ok"; else echo "  FAIL"; FAIL=1; fi

[ "$FAIL" = 0 ] && echo "== PASS ==" || { echo "== FAIL =="; exit 1; }

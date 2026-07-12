#!/usr/bin/env bash
# Regenerates data/cert/x509_crt_bundle.bin from the current Mozilla CA bundle.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUNDLE_OUT="$REPO_ROOT/data/cert/x509_crt_bundle.bin"
CACERT_URL="https://curl.se/ca/cacert.pem"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Downloading latest Mozilla CA bundle from $CACERT_URL ..."
curl -sf --max-time 30 "$CACERT_URL" -o "$TMP_DIR/cacert.pem"

echo "Regenerating x509 certificate bundle..."
(cd "$TMP_DIR" && python3 "$SCRIPT_DIR/gen_crt_bundle.py" -i "$TMP_DIR/cacert.pem" "$SCRIPT_DIR/extra_roots" -q)

OLD_COUNT=0
if [ -f "$BUNDLE_OUT" ]; then
  OLD_COUNT=$(python3 -c "import struct; print(struct.unpack('>H', open('$BUNDLE_OUT','rb').read(2))[0])")
fi

mv "$TMP_DIR/x509_crt_bundle" "$BUNDLE_OUT"
NEW_COUNT=$(python3 -c "import struct; print(struct.unpack('>H', open('$BUNDLE_OUT','rb').read(2))[0])")

echo "Done: $BUNDLE_OUT ($OLD_COUNT -> $NEW_COUNT root CAs)"

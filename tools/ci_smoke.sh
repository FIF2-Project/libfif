#!/usr/bin/env bash
# ------------------------------------------------------------------------
# libfif v3.5 / fif v2.5 - FIF2 fractal image format
#
# libfif and fif:  Oskar Behzadpour
# FIF2 format:     Rezar Behzad
# Web:             www.FractalImageCompression.com
# Contact:         oskar@fractalimagecompression.com
#
# Copyright (c) 2026 Oskar Behzadpour
# ------------------------------------------------------------------------

# Smoke test used by CI: encode, inspect and decode a generated picture.
#   tools/ci_smoke.sh ./fif        (or ./fif.exe)
# Fails on the first command that returns non-zero.
set -euo pipefail

FIF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

PY="$(command -v python3 || command -v python)"

# 256x192 RGB test picture: gradients plus a few hard edges.
"$PY" - <<'PYEOF'
w, h = 256, 192
px = bytearray()
for y in range(h):
    for x in range(w):
        edge = 255 if (x // 32 + y // 32) % 2 and x > 128 else 0
        px += bytes((x % 256, (y * 4 // 3) % 256, (edge + (x ^ y)) % 256))
open("test.ppm", "wb").write(b"P6\n%d %d\n255\n" % (w, h) + px)
PYEOF

echo "== version";  "$FIF" version
"$FIF" version | grep -q "PNG"  || { echo "error: built without PNG";  exit 1; }
"$FIF" version | grep -q "JPEG" || { echo "error: built without JPEG"; exit 1; }

echo "== encode";   "$FIF" encode test.ppm test.fif -q 70 --no-progress
echo "== info";     "$FIF" info test.fif
echo "== decode";   "$FIF" decode test.fif back.ppm
                    "$FIF" decode test.fif back.png
                    "$FIF" decode test.fif big.png --scale 2
echo "== compare";  "$FIF" compare test.ppm back.ppm

for f in test.fif back.ppm back.png big.png; do
  [ -s "$f" ] || { echo "error: $f missing or empty"; exit 1; }
done
echo "smoke test passed"

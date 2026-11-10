#!/bin/sh
# ------------------------------------------------------------------------
# libfif v3.5 / fif v2.5 - FIF2 fractal image format
#
# libfif and fif:  Oskar Behzadpour
# FIF2 format:     Rezar Behzad
# Web:             www.FractalImageCompression.com
# Contact:         oskar@fractalimagecompression.com
#
# Copyright (c) 2026 Oskar Behzadpour
# SPDX-License-Identifier: MIT
# ------------------------------------------------------------------------

# Rebuilds src/ctxtable.inc from the context statistics of a training set.
# Run after any change to the context layout; then rebuild.
set -e
TMP="${TMPDIR:-/tmp}"
OUT="$TMP/ctxtrain.txt"
ACC="$TMP/ctxaccum.txt"
# Both, not just the per-run file: an accumulator left over from a build with a
# different context layout aborts the merge halfway through.
rm -f "$OUT" "$ACC"
for img in "$@"; do
  for q in 25 45 65 80 92; do
    for sub in 420 444; do
      ./fif encode "$img" "$TMP/train.fif" -y -q $q -e 4 --subsampling $sub \
            --train-contexts "$OUT" --no-progress >/dev/null 2>&1 || true
      python3 tools/merge_contexts.py "$OUT" "$ACC"
    done
  done
done
python3 tools/merge_contexts.py --emit "$ACC" src/ctxtable.inc
echo "wrote src/ctxtable.inc"

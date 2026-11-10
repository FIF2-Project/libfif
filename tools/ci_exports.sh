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
# SPDX-License-Identifier: MIT
# ------------------------------------------------------------------------

# Checks the export table of the shared library:
#   tools/ci_exports.sh libfif.dll | libfif.3.dylib | libfif.so.3
# Every FIF_API function in include/fif/fif.h must be exported, and nothing
# from the internal namespace fif:: may leak out.
set -euo pipefail
LIB="$1"
HDR="$(dirname "$0")/../include/fif/fif.h"

expected="$(grep -E '^FIF_API' "$HDR" | grep -oE 'fif_[a-z_]+\(' | tr -d '(' | sort -u)"

case "$LIB" in
  *.dll)   all="$(llvm-readobj --coff-exports "$LIB" | sed -n 's/^ *Name: //p')" ;;
  *.dylib) all="$(nm -gU "$LIB" | awk '{print $3}' | sed 's/^_//')" ;;
  *)       all="$(nm -D --defined-only "$LIB" | awk '{print $3}')" ;;
esac

exported="$(printf '%s\n' "$all" | grep -E '^fif_' | sort -u || true)"
missing="$(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$exported"))"
extra="$(comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$exported"))"
leaked="$(printf '%s\n' "$all" | grep -E '^_?_ZN3fif' || true)"

echo "$(printf '%s\n' "$expected" | wc -l | tr -d ' ') API functions, $(printf '%s\n' "$exported" | grep -c . || true) fif_* exports, $(printf '%s\n' "$all" | grep -c . || true) exports in total"
[ -z "$missing" ] || { echo "error: not exported:";        echo "$missing"; exit 1; }
[ -z "$extra" ]   || { echo "error: exported but not API:"; echo "$extra";   exit 1; }
[ -z "$leaked" ]  || { echo "error: internal symbols leak:"; echo "$leaked" | head; exit 1; }
echo "exports ok"

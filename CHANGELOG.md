# Changelog

All notable changes to libfif and the fif tool are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Releases are tagged twice, `libfif-vX.Y.Z` and `fif-vA.B.C`, on the same commit.

## [Unreleased]

## [libfif 3.5.0 / fif 2.5.0] - YYYY-MM-DD

First public release.

- FIF2 format 3.3: header, sections, chunks, zlib-compressed sections, CRCs.
- Encoder: rate-distortion quadtree, local-window and kd-tree domain search,
  per-tile strategy probe, planar leaves, shared and from-luma chroma,
  4:4:4 / 4:2:0 / 4:0:0 subsampling, quality / PSNR / bpp targets.
- Decoder: DC-seeded iteration, in-loop seam filter, arbitrary output scale,
  progressive prefixes, per-tile decoding, damaged-file tolerance.
- `fif` tool: `encode`, `decode`, `info`, `thumbnail`, `compare`, `bench`,
  `version`, `debug`.
- Prebuilt binaries for Windows x64 and macOS universal, plus SDK packages.

[Unreleased]: https://github.com/FIF2-Project/libfif/compare/libfif-v3.5.0...HEAD
[libfif 3.5.0 / fif 2.5.0]: https://github.com/FIF2-Project/libfif/releases/tag/libfif-v3.5.0

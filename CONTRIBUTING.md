# Contributing to libfif

Thank you for helping. This page explains how to report problems and how to
get a change merged.

## Where does my question go?

| Topic | Place |
| --- | --- |
| Bug in the encoder, decoder or `fif` tool | [Issues](https://github.com/FIF2-Project/libfif/issues) in this repository |
| Question or proposal about the **file format** | the FIF2 specification at [fif2.org](https://fif2.org) |
| General information, news, contact | [fractalimagecompression.com](https://www.fractalimagecompression.com) |

## Reporting a bug

Use the bug report template and include:

- the output of `fif version` (it shows the build, SIMD level and image libraries),
- the exact command you ran and what happened,
- if possible, the input picture or `.fif` file. A file that decodes wrongly
  is the most useful thing you can attach.

## Building and testing

See [docs/getting-started.md](docs/getting-started.md). Before you open a
pull request, please check at least:

```sh
make clean && make            # or: build.bat clean & build.bat
./fif bench <some picture> -q 60
make san                      # sanitiser build; run an encode and a decode with it
```

CI builds and smoke-tests every pull request on Windows, macOS and Linux.

## Guidelines for changes

- **Keep the bitstream in mind.** A change to what the encoder writes must
  still decode with the current decoder, or it is a format change. Format
  changes go through the specification first ([fif2.org](https://fif2.org)).
- **Measure.** Changes to the codec should state their effect at a matched rate
  (PSNR / MS-SSIM against file size, e.g. from `fif bench`), and their speed impact.
- **Contexts and trained tables go together.** If you change the context layout,
  rerun `tools/train_contexts.sh` and commit the new `src/ctxtable.inc` in the
  same pull request.
- **CLI, help text and docs together.** A new or changed option needs its
  `fif help` text and its entry in [docs/cli.md](docs/cli.md) updated as well.
- **Style.** C++20, 4-space indentation, warnings clean with
  `-Wall -Wextra`. Follow the style of the surrounding code. New source files
  start with the same banner comment as the existing ones.
- **One topic per pull request.** Small pull requests get reviewed faster.

## Licence of contributions

The project licence has not been published yet. Until it is, please open an
issue before investing in a larger contribution.

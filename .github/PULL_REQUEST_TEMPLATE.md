## What does this change?

## Why?

## Measurements
<!-- For codec changes: file size and PSNR / MS-SSIM at a matched rate, and speed. Use `fif bench`. -->

## Checklist
- [ ] Builds without warnings (`make` / `build.bat`)
- [ ] Encode + decode tested, also with the sanitiser build (`make san`)
- [ ] Existing `.fif` files still decode (no unintended format change)
- [ ] `fif help` text updated for new or changed options (the docs on fif2.org follow separately)
- [ ] `src/ctxtable.inc` retrained if the context layout changed
- [ ] `CHANGELOG.md` updated under "Unreleased"

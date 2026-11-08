#!/usr/bin/env python3
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
"""Packages already-built (and signed) release binaries into the release zips.

Run from the repository root, after the build (and signing) steps:

    python tools/package_release.py windows-x64      [--vcpkg DIR] [--out dist]
    python tools/package_release.py macos-universal  [--vcpkg DIR] [--out dist]
    python tools/package_release.py finalize         [--out dist]

windows-x64      expects fif.exe, libfif.dll, libfif.lib, libfif.pdb and
                 libfif_static.lib in the repository root (build.bat release
                 generic shared), and the static image libraries in vcpkg's
                 x64-windows-static tree.
macos-universal  expects the lipo-merged fif, libfif.3.dylib, libfif.a,
                 libpng16.a and libjpeg.a in the repository root (see the
                 release workflow), and vcpkg's arm64-osx tree for the notices.
finalize         writes SHA256SUMS.txt over every zip in the output folder and
                 RELEASE_NOTES.md from CHANGELOG.md.  Run it once all zips of
                 the release are in one folder.

Each platform step checks that the binaries report the expected versions and
a generic SIMD level before it packages anything.  --require-signed also checks
the code signatures, and should be used for real releases.

Standard library only.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile
from typing import NoReturn

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
REPO_URL = 'https://github.com/FIF2-Project/libfif'

# The IJG licence that libjpeg-turbo's copyright file refers to ("README.ijg")
# is not installed by vcpkg.  Its legal section is reproduced here verbatim
# (libjpeg-turbo 3.2.0), because condition (2) applies to every binary we ship.
IJG_LEGAL = """\
LEGAL ISSUES
============

In plain English:

1. We don't promise that this software works.  (But if you find any bugs,
   please let us know!)
2. You can use this software for whatever you want.  You don't have to pay us.
3. You may not pretend that you wrote this software.  If you use it in a
   program, you must acknowledge somewhere in your documentation that
   you've used the IJG code.

In legalese:

The authors make NO WARRANTY or representation, either express or implied,
with respect to this software, its quality, accuracy, merchantability, or
fitness for a particular purpose.  This software is provided "AS IS", and you,
its user, assume the entire risk as to its quality and accuracy.

This software is copyright (C) 1991-2020, Thomas G. Lane, Guido Vollbeding.
All Rights Reserved except as specified below.

Permission is hereby granted to use, copy, modify, and distribute this
software (or portions thereof) for any purpose, without fee, subject to these
conditions:
(1) If any part of the source code for this software is distributed, then this
README file must be included, with this copyright and no-warranty notice
unaltered; and any additions, deletions, or changes to the original files
must be clearly indicated in accompanying documentation.
(2) If only executable code is distributed, then the accompanying
documentation must state that "this software is based in part on the work of
the Independent JPEG Group".
(3) Permission for use of this software is granted only if the user accepts
full responsibility for any undesirable consequences; the authors accept
NO LIABILITY for damages of any kind.

These conditions apply to any software derived from or based on the IJG code,
not just to the unmodified library.  If you use our work, you ought to
acknowledge us.

Permission is NOT granted for the use of any IJG author's name or company name
in advertising or publicity relating to this software or products derived from
it.  This software may be referred to only as "the Independent JPEG Group's
software".

We specifically permit and encourage the use of this software as the basis of
commercial products, provided that all warranty or liability claims are
assumed by the product vendor.
"""

IJG_ACK = 'This software is based in part on the work of the Independent JPEG Group.'


def die(msg) -> NoReturn:
    sys.exit('package_release: error: ' + msg)


def warn(msg):
    print('package_release: WARNING: ' + msg, file=sys.stderr)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


# ------------------------------------------------------------------ versions

def versions(args):
    hdr = read(os.path.join(ROOT, 'include', 'fif', 'fif.h'))
    cli = read(os.path.join(ROOT, 'tools', 'fif_cli.cpp'))
    fmt = read(os.path.join(ROOT, 'src', 'format.h'))

    def grab(text, pattern):
        m = re.search(pattern, text)
        if not m:
            die('cannot find %r' % pattern)
        return m.group(1)

    lib = '%s.%s' % (grab(hdr, r'#define FIF_VERSION_MAJOR (\d+)'),
                     grab(hdr, r'#define FIF_VERSION_MINOR (\d+)'))
    tool = '%s.%s' % (grab(cli, r'kCliVersionMajor = (\d+)'),
                      grab(cli, r'kCliVersionMinor = (\d+)'))
    form = '%s.%s' % (grab(fmt, r'kVerMajor = (\d+)'), grab(fmt, r'kVerMinor = (\d+)'))
    v = {
        'lib_mm': lib, 'cli_mm': tool, 'format': form,
        'lib': args.lib_version or lib + '.0',
        'cli': args.cli_version or tool + '.0',
    }
    for key in ('lib', 'cli'):
        mm = v[key + '_mm']
        if not v[key].startswith(mm + '.'):
            die('--%s-version %s does not match %s %s in the sources'
                % (key, v[key], 'libfif' if key == 'lib' else 'fif', mm))
    return v


def check_binary(exe, v, simd, prefix=()):
    """Runs `fif version` and checks the versions, the SIMD level and image I/O."""
    out = ''
    try:
        out = subprocess.run(list(prefix) + [exe, 'version'], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        die('cannot run %s version: %s' % (exe, e))
    want = [
        (r'^\s*fif(\.exe)?\s+%s\s' % re.escape(v['cli_mm']), 'fif %s' % v['cli_mm']),
        (r'^\s*libfif\s+%s\s' % re.escape(v['lib_mm']), 'libfif %s' % v['lib_mm']),
        (r'SIMD %s\b' % simd, 'SIMD %s (a generic build)' % simd),
        (r'PNG', 'PNG support'), (r'JPEG', 'JPEG support'), (r'zlib', 'zlib support'),
    ]
    for pattern, what in want:
        if not re.search(pattern, out, re.M):
            die('%s does not report %s:\n%s' % (exe, what, out))
    print('  checked   %s %s: versions, SIMD %s, PNG/JPEG/zlib'
          % (' '.join(prefix), os.path.basename(exe), simd))


def check_signed(paths, platform):
    for p in paths:
        if platform == 'windows-x64':
            cmd = ['signtool', 'verify', '/pa', '/q', p]
        else:
            cmd = ['codesign', '--verify', '--strict', p]
        if subprocess.run(cmd, capture_output=True).returncode != 0:
            die('%s is not validly signed (%s)' % (p, ' '.join(cmd[:2])))
        print('  signed    ' + os.path.basename(p))


# ------------------------------------------------------------------ texts

def notices(copyrights, system_zlib):
    parts = [
        'THIRD-PARTY NOTICES',
        '===================',
        '',
        'libfif and fif include the following third-party software, linked',
        'statically.  Their licences follow.',
        '',
        IJG_ACK,
        '',
    ]
    if system_zlib:
        parts += ['zlib is not included: on macOS, the system zlib is used.', '']
    for name, path in copyrights:
        parts += ['', '-' * 78, name, '-' * 78, '', read(path).rstrip(), '']
        if name == 'libjpeg-turbo':
            parts += ['', 'The IJG License (README.ijg of libjpeg-turbo, legal section):',
                      '', IJG_LEGAL.rstrip(), '']
    return '\n'.join(parts) + '\n'


def links():
    return ('- Using the fif tool: https://fif2.org/lib-cli.html\n'
            '- Using the library: https://fif2.org/lib-api.html\n'
            '- The FIF2 format: https://fif2.org\n'
            '- What is fractal image compression? https://www.fractalimagecompression.com\n'
            '- Source code and issues: %s\n' % REPO_URL)


def cli_readme(v, platform):
    exe = 'fif.exe' if platform == 'windows-x64' else './fif'
    if platform == 'windows-x64':
        install = ('Unzip the folder and run `fif.exe` from a terminal (PowerShell or\n'
                   'Command Prompt). To use it from anywhere, add the folder to your PATH.\n'
                   'It needs no other files.\n')
    else:
        install = ('Unzip and run `./fif` from Terminal. To use it from anywhere, move it\n'
                   'to a folder on your PATH, e.g. `sudo mv fif /usr/local/bin/`.\n'
                   'It runs natively on Apple Silicon and on Intel Macs (macOS 11 or later).\n'
                   'If macOS refuses to open it, run `xattr -d com.apple.quarantine fif`.\n')
    return f"""# fif {v['cli']}: FIF2 command-line tool

Part of libfif {v['lib']} · FIF2 format {v['format']}

`fif` converts pictures to the FIF2 fractal image format and back, shows what a
FIF2 file contains, and measures quality and speed.

## Installing

{install}
## Quick start

```
{exe} encode photo.jpg photo.fif -q 80
{exe} decode photo.fif photo.png
{exe} decode photo.fif big.png --scale 4
{exe} info   photo.fif
{exe} help
```

## More

{links()}
## Third-party software

{IJG_ACK} See THIRD-PARTY-NOTICES.txt for the licences of the included
libraries.
"""


def sdk_readme(v, platform):
    if platform == 'windows-x64':
        table = """| Use | Files |
| --- | --- |
| **shared (recommended)**: any compiler setting, Debug or Release, any language | `bin/libfif.dll`, `lib/libfif.lib` (import library) |
| static, single executable, **only for `/MT` Release** programs | `lib/static/libfif_static.lib` + `lib/static/libpng16.lib`, `jpeg.lib`, `zs.lib` |
"""
        build = """```
clang -O2 -Iinclude myprogram.c lib/libfif.lib -o myprogram.exe
cl /O2 /Iinclude myprogram.c lib\\libfif.lib
```

Ship `libfif.dll` next to your program. `libfif.pdb` holds its debug symbols.

Static: `cl /MT /O2 /Iinclude myprogram.c lib\\static\\libfif_static.lib
lib\\static\\libpng16.lib lib\\static\\zs.lib lib\\static\\jpeg.lib`. For
`/MD` or Debug builds, use the DLL."""
    else:
        table = """| Use | Files |
| --- | --- |
| **shared (recommended)**: any language with a C FFI | `lib/libfif.3.dylib` (`lib/libfif.dylib` links to it) |
| static, single executable | `lib/static/libfif.a` + `lib/static/libpng16.a`, `libjpeg.a`, system `-lz` |
"""
        build = """```
clang -O2 -Iinclude myprogram.c -Llib -lfif -Wl,-rpath,@loader_path -o myprogram
```

Ship `libfif.3.dylib` next to your program (or in your app bundle's
Frameworks folder with a matching rpath). Its install name is
`@rpath/libfif.3.dylib`. Both slices, arm64 and x86_64, are included.

Static: `clang++ -O2 -Iinclude myprogram.c lib/static/libfif.a
lib/static/libpng16.a lib/static/libjpeg.a -lz -o myprogram`."""
    return f"""# libfif {v['lib']} SDK

The FIF2 encoder and decoder as a C library. libfif {v['lib']} · fif
{v['cli']} · FIF2 format {v['format']}

## Which file do I need?

{table}
The shared library has libpng, libjpeg-turbo{' and zlib' if platform == 'windows-x64' else ''} built in and exports only the
`fif_*` functions of `include/fif/fif.h`. Nothing needs to be defined to use it.
The bundled static image libraries can be left out if your program already
links its own.

## Building against it

{build}

## Documentation

{links()}
## Third-party software

{IJG_ACK} See THIRD-PARTY-NOTICES.txt.
"""


# ------------------------------------------------------------------ zipping

def write_zip(path, top, entries):
    """entries: (archive name, source path or None, text or None, mode, symlink target)."""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, src, text, mode, link in entries:
            info = zipfile.ZipInfo(top + '/' + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # unix, so that the mode bits are honoured
            if link:
                info.external_attr = (0o120777 << 16)
                z.writestr(info, link)
                continue
            info.external_attr = ((0o100000 | mode) << 16)
            if text is not None:
                z.writestr(info, text.replace('\r\n', '\n'))
            else:
                with open(src, 'rb') as f:
                    z.writestr(info, f.read())
    print('  wrote     ' + os.path.relpath(path, ROOT))


def common_docs(readme, notice_text):
    out: list = [('README.md', None, readme, 0o644, None),
           ('THIRD-PARTY-NOTICES.txt', None, notice_text, 0o644, None)]
    lic = os.path.join(ROOT, 'LICENSE')
    if os.path.exists(lic):
        out.append(('LICENSE', lic, None, 0o644, None))
    else:
        warn('no LICENSE file in the repository: the zips go out without one')
    return out


def need(paths):
    missing = [p for p in paths if not os.path.exists(p)]
    if missing:
        die('missing build output:\n  ' + '\n  '.join(missing))


def find_vcpkg(arg):
    for c in (arg, os.environ.get('VCPKG_ROOT'), os.path.expanduser('~/vcpkg'), 'C:/vcpkg'):
        if c and os.path.isdir(os.path.join(c, 'installed')):
            return c
    die('vcpkg not found; pass --vcpkg')


def package_windows(args, v):
    r = lambda *p: os.path.join(ROOT, *p)
    vc = os.path.join(find_vcpkg(args.vcpkg), 'installed', 'x64-windows-static')
    bins = [r('fif.exe'), r('libfif.dll'), r('libfif.lib'), r('libfif.pdb'), r('libfif_static.lib')]
    deps = [os.path.join(vc, 'lib', n) for n in ('libpng16.lib', 'jpeg.lib', 'zs.lib')]
    cr = [(n, os.path.join(vc, 'share', n, 'copyright')) for n in ('libpng', 'libjpeg-turbo', 'zlib')]
    need(bins + deps + [p for _, p in cr])

    check_binary(r('fif.exe'), v, 'sse2')
    if args.require_signed:
        check_signed([r('fif.exe'), r('libfif.dll')], 'windows-x64')
    note = notices(cr, system_zlib=False)
    os.makedirs(args.out, exist_ok=True)

    top = 'fif-%s-windows-x64' % v['cli']
    write_zip(os.path.join(args.out, top + '.zip'), top,
              [('fif.exe', r('fif.exe'), None, 0o755, None)]
              + common_docs(cli_readme(v, 'windows-x64'), note))

    top = 'libfif-%s-sdk-windows-x64' % v['lib']
    write_zip(os.path.join(args.out, top + '.zip'), top, [
        ('include/fif/fif.h', r('include', 'fif', 'fif.h'), None, 0o644, None),
        ('bin/libfif.dll', r('libfif.dll'), None, 0o755, None),
        ('bin/libfif.pdb', r('libfif.pdb'), None, 0o644, None),
        ('lib/libfif.lib', r('libfif.lib'), None, 0o644, None),
        ('lib/static/libfif_static.lib', r('libfif_static.lib'), None, 0o644, None),
    ] + [('lib/static/' + os.path.basename(d), d, None, 0o644, None) for d in deps]
      + common_docs(sdk_readme(v, 'windows-x64'), note))


def package_macos(args, v):
    r = lambda *p: os.path.join(ROOT, *p)
    vc = os.path.join(find_vcpkg(args.vcpkg), 'installed', 'arm64-osx')
    bins = [r('fif'), r('libfif.3.dylib'), r('libfif.a'), r('libpng16.a'), r('libjpeg.a')]
    cr = [(n, os.path.join(vc, 'share', n, 'copyright')) for n in ('libpng', 'libjpeg-turbo')]
    need(bins + [p for _, p in cr])

    for f in (r('fif'), r('libfif.3.dylib')):
        archs = subprocess.run(['lipo', '-archs', f], capture_output=True, text=True).stdout.split()
        if sorted(archs) != ['arm64', 'x86_64']:
            die('%s is not universal (lipo -archs: %s)' % (f, ' '.join(archs) or '?'))
    check_binary(r('fif'), v, 'neon')
    try:
        check_binary(r('fif'), v, 'sse2', prefix=('arch', '-x86_64'))
    except SystemExit:
        warn('could not run the x86_64 slice (is Rosetta installed?); not checked')
    if args.require_signed:
        check_signed([r('fif'), r('libfif.3.dylib')], 'macos-universal')
    note = notices(cr, system_zlib=True)
    os.makedirs(args.out, exist_ok=True)

    top = 'fif-%s-macos-universal' % v['cli']
    write_zip(os.path.join(args.out, top + '.zip'), top,
              [('fif', r('fif'), None, 0o755, None)]
              + common_docs(cli_readme(v, 'macos-universal'), note))

    top = 'libfif-%s-sdk-macos-universal' % v['lib']
    write_zip(os.path.join(args.out, top + '.zip'), top, [
        ('include/fif/fif.h', r('include', 'fif', 'fif.h'), None, 0o644, None),
        ('lib/libfif.3.dylib', r('libfif.3.dylib'), None, 0o755, None),
        ('lib/libfif.dylib', None, None, 0, 'libfif.3.dylib'),
        ('lib/static/libfif.a', r('libfif.a'), None, 0o644, None),
        ('lib/static/libpng16.a', r('libpng16.a'), None, 0o644, None),
        ('lib/static/libjpeg.a', r('libjpeg.a'), None, 0o644, None),
    ] + common_docs(sdk_readme(v, 'macos-universal'), note))


def finalize(args, v):
    zips = sorted(f for f in os.listdir(args.out) if f.endswith('.zip')) if os.path.isdir(args.out) else []
    if not zips:
        die('no zips in ' + args.out)
    expected = ['fif-%s-windows-x64.zip' % v['cli'], 'fif-%s-macos-universal.zip' % v['cli'],
                'libfif-%s-sdk-windows-x64.zip' % v['lib'], 'libfif-%s-sdk-macos-universal.zip' % v['lib']]
    for e in expected:
        if e not in zips:
            warn('%s is missing from %s' % (e, args.out))
    with open(os.path.join(args.out, 'SHA256SUMS.txt'), 'w', newline='\n') as f:
        for z in zips:
            h = hashlib.sha256()
            with open(os.path.join(args.out, z), 'rb') as zf:
                for block in iter(lambda: zf.read(1 << 20), b''):
                    h.update(block)
            f.write('%s  %s\n' % (h.hexdigest(), z))
    print('  wrote     ' + os.path.relpath(os.path.join(args.out, 'SHA256SUMS.txt'), ROOT))

    log = read(os.path.join(ROOT, 'CHANGELOG.md'))
    head = '## [libfif %s / fif %s]' % (v['lib'], v['cli'])
    i = log.find(head)
    if i < 0:
        die('CHANGELOG.md has no section "%s"' % head)
    body = log[log.index('\n', i) + 1:]
    j = re.search(r'^## \[|^\[', body, re.M)
    changes = body[:j.start()].strip() if j else body.strip()
    if re.search(r'YYYY-MM-DD', log[i:log.index('\n', i)]):
        warn('the CHANGELOG section still has the date YYYY-MM-DD')

    def dl(name, what):
        return '- %s: [%s](%s/releases/download/libfif-v%s/%s)' % (what, name, REPO_URL, v['lib'], name)
    notes = '\n'.join([
        '**libfif %s** · **fif %s** · FIF2 format %s' % (v['lib'], v['cli'], v['format']),
        '',
        'The FIF2 format: https://fif2.org · What is fractal image compression? '
        'https://www.fractalimagecompression.com',
        '',
        '## Downloads',
        dl(expected[0], 'Windows (x64), command-line tool'),
        dl(expected[1], 'macOS (Apple Silicon + Intel), command-line tool'),
        dl(expected[2], 'Windows SDK (DLL + static library)'),
        dl(expected[3], 'macOS SDK (dylib + static library)'),
        '',
        'Verify downloads with `SHA256SUMS.txt`. Documentation: '
        '[Getting started](https://fif2.org/lib-getting-started.html) · '
        '[Using the fif tool](https://fif2.org/lib-cli.html) · '
        '[Using the library](https://fif2.org/lib-api.html)',
        '',
        '## Changes',
        '',
        changes,
        '',
    ])
    with open(os.path.join(args.out, 'RELEASE_NOTES.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(notes)
    print('  wrote     ' + os.path.relpath(os.path.join(args.out, 'RELEASE_NOTES.md'), ROOT))


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or '').split('\n')[0])
    ap.add_argument('step', choices=['windows-x64', 'macos-universal', 'finalize'])
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--vcpkg', help='vcpkg root (default: VCPKG_ROOT, ~/vcpkg, C:/vcpkg)')
    ap.add_argument('--lib-version', help='full libfif version, default <major>.<minor>.0')
    ap.add_argument('--cli-version', help='full fif version, default <major>.<minor>.0')
    ap.add_argument('--require-signed', action='store_true',
                    help='fail unless fif and the shared library carry valid signatures')
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    v = versions(args)
    print('libfif %s / fif %s / FIF2 %s: %s' % (v['lib'], v['cli'], v['format'], args.step))
    {'windows-x64': package_windows, 'macos-universal': package_macos,
     'finalize': finalize}[args.step](args, v)


if __name__ == '__main__':
    main()

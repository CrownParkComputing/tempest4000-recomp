#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assembles the release folder (dist/) from the build, and optionally packs it.

    python tools/package.py [--out dist] [--zip] [--debug]

The same folder holds tempest and tempest-setup (rpath $ORIGIN/lib), the shared libraries they
need in lib/ (ldd, less the host's: glibc, libstdc++, Vulkan, the display and audio stacks, which
must match its drivers), tempest.png for the desktop entry and prepare/ without Python: the setup
runs the system's python3. --zip writes <release>-linux.tar.gz (and -symbols, -debug), which keeps
the files' exec bits and symlinks. Needs ldd, patchelf and strip.

The folder is what a player unpacks: they need neither this repo nor Python of their own beyond
python3. They run tempest-setup, which prepares the game image from their own .pkg or dump
(prepare/run.py), and then start tempest or the desktop entry it offers.

    tempest, tempest-setup   debug info stripped
    lib/            the shared libraries they need, with their own RUNPATH
    release.txt     this release; the setup stamps the image it prepares with it (src/launcher.c)
    recomplauncher.json  how RecompLauncher (recomplauncher.com) sets up, starts and configures it
    README.txt, LICENSE.txt
    prepare/        run.py, scripts/ and settings.example.ini, run by the setup

--debug keeps the debug info in the binaries instead. A plain --zip also writes
<release>-symbols.tar.gz with the unstripped binaries, which resolve the offsets in a player's
tempest.log.

What the setup adds later (game/, paths.ini, settings.ini) is left alone, so dist/ also works as
the developer's own install. Never ship game/: it is built from a player's own .pkg or dump.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

PORT = Path(__file__).resolve().parent.parent
EXES = ('tempest', 'tempest-setup')
# Linux libraries the host provides: bundled, they break its GPU drivers, display connection or audio
# (libstdc++ too: Mesa needs the one it was built with, at least as new as this build's).
HOST_LIBS = re.compile(r'^(linux-vdso|ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl'
                       r'|libstdc\+\+|libgcc_s|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*'
                       r'|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor'
                       r'|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype'
                       r'|libnvidia-.*|libcuda|libz)\.so')
ICON = PORT / 'res' / 'tempest.ico'
# What prepare/run.py needs: the scripts and the template of settings.ini.
PREPARE_FILES = ['run.py', 'settings.example.ini', *sorted(f'scripts/{p.name}' for p in (PORT / 'scripts').glob('*.py'))]
README = """Tempest on PC (Linux)
Release {release}

You need the .pkg of your own copy of the PS4 game (CUSA00038, version 1.10). The setup extracts
it into a folder next to it (a few seconds, about 700 MB); a dump you extracted yourself works
too, the folder with eboot.bin, sce_module, sce_sys, audio.pck and stream.dat. No game files are
included. Add-ons you own (ships and skins) need no files: a DLC folder with their sce_sys folders
lists them.

1. Run tempest-setup (from a file manager, or ./tempest-setup in a terminal). Pick your .pkg
   (or a dump you already have) and where your saves go. It prepares the game and offers an
   entry in your application menu (Steam's "Add a Non-Steam Game" lists it too).
   From a terminal it also takes: ./tempest-setup --game <pkg or dump> [--save <folder>]
2. Start the game from the menu entry, or run ./tempest.

Run tempest-setup again to change the game or save folder. After unpacking a newer release
over this folder, run it once more: it keeps your folders and settings.

In the game: F10, or Select (Share/View) held for two seconds, opens the display settings (window,
resolution, VSync, 60/90/120/144/180 FPS); F4, or Back on the title screen, asks to close the game.

settings.ini (made by the setup) holds the window, audio and control options; one in your save
folder is read instead, and the display settings save there. If the game stops, the reason is in
tempest.log, in the log folder of your save folder. Shader caches are kept in the shaders
folder next to tempest-setup.

Needs Linux x86-64 with glibc 2.39 or newer (SteamOS 3.7, Ubuntu 24.04, Debian 13, Fedora 40 or
newer), python3 3.10+ for the setup, a CPU with AVX and a GPU with a Vulkan 1.3 driver.
Source and license (GPL-2.0-or-later): see LICENSE.txt.
"""


def pe_imports(path):
    """The DLLs a PE file imports, normal and delay-loaded."""
    data = path.read_bytes()
    fh = struct.unpack_from('<I', data, 0x3C)[0] + 4
    sections, opt_size = struct.unpack_from('<H', data, fh + 2)[0], struct.unpack_from('<H', data, fh + 16)[0]
    opt = fh + 20
    if struct.unpack_from('<H', data, opt)[0] != 0x20B:
        raise SystemExit(f'{path}: not a 64-bit PE file')
    table = opt + opt_size

    def offset(rva):
        for i in range(sections):
            vsize, va, raw, ptr = struct.unpack_from('<4I', data, table + i * 40 + 8)
            if va <= rva < va + max(vsize, raw):
                return ptr + rva - va
        raise SystemExit(f'{path}: bad import table')

    names = []
    # Import descriptors are 20 bytes with the name at +12; delay-load descriptors 32 with it at +4.
    for directory, size, name_at in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from('<I', data, opt + 112 + directory * 8)[0]
        if not rva:
            continue
        at = offset(rva)
        while (name := struct.unpack_from('<I', data, at + name_at)[0]):
            start = offset(name)
            names.append(data[start:data.index(b'\0', start)].decode('ascii'))
            at += size
    return names


def linux_tool(*names):
    for name in names:
        if (path := shutil.which(name)):
            return path
    raise SystemExit(f'Need {names[0]} (.github/scripts/ci-deps.sh installs it).')


def so_closure(exes):
    """The shared libraries `exes` load, theirs included (ldd lists them all), less the host's."""
    found, missing = {}, []
    for exe in exes:
        listing = subprocess.run(['ldd', str(exe)], capture_output=True, text=True, check=True).stdout
        for name, path in re.findall(r'^\s*(\S+)\s+=>\s+(\S+)', listing, re.MULTILINE):
            if HOST_LIBS.match(name):
                continue
            if path == 'not':
                missing.append(name)
            else:
                found[name] = Path(path)
    if missing:
        raise SystemExit(f'ldd cannot find {", ".join(sorted(set(missing)))}')
    return [found[name] for name in sorted(found)]


def set_rpath(path, rpath):
    """Bundled libraries find each other through their own RUNPATH: the exe's applies to its direct
    dependencies only."""
    subprocess.run([linux_tool('patchelf'), '--set-rpath', rpath, str(path)], check=True)


def min_glibc(paths):
    """The newest GLIBC_ symbol version the files use: the oldest glibc they start on."""
    objdump = shutil.which('objdump') or shutil.which('llvm-objdump-18')
    if not objdump:
        return None
    versions = set()
    for path in paths:
        dump = subprocess.run([objdump, '-T', str(path)], capture_output=True, text=True).stdout
        versions |= set(re.findall(r'GLIBC_(2\.[0-9.]+)', dump))
    return max(versions, key=lambda v: tuple(map(int, v.split('.'))), default=None)


def ico_png(ico, size=256):
    """The PNG frame of `size` pixels in a .ico (the desktop entry's icon)."""
    data = ico.read_bytes()
    for i in range(struct.unpack_from('<H', data, 4)[0]):
        width, _, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        if (width or 256) == size and data[offset:offset + 8] == b'\x89PNG\r\n\x1a\n':
            return data[offset:offset + length]
    raise SystemExit(f'{ico}: no {size}px PNG frame')


def release_name(product='tempest-pc'):
    """<product>-<commit>-<date>, or -<commit>-dirty-<date>-<time> for uncommitted work:
    what release.txt and the zip are named after. Two releases from one dirty tree differ by the
    packaging time, so the launcher asks for the setup again after either is unpacked."""
    def git(*args):
        return subprocess.run(['git', *args], cwd=PORT, capture_output=True, text=True).stdout.strip()
    described = git('describe', '--tags', '--always', '--dirty') or 'unknown'
    when = time.strftime('%Y-%m-%d-%H%M') if described.endswith('-dirty') else git('log', '-1', '--format=%cs')
    return f'{product}-{described}-{when}'


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def strip(source, target, mode='--strip-all'):
    """Copies an exe without its debug info (RelWithDebInfo DWARF is ~95% of tempest.exe).
    Crash logs give exe+offset, which the unstripped build resolves."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tool = linux_tool('llvm-strip-18', 'llvm-strip', 'strip')
    subprocess.run([tool, mode, '-o', str(target), str(source)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, help='the folder to fill (default dist)')
    p.add_argument('--zip', action='store_true', help='also write out/<release>.zip (and <release>-symbols.zip)')
    p.add_argument('--debug', action='store_true', help='keep debug info in the exes and ship the diagnose/ kit')
    a = p.parse_args()
    for exe in EXES:
        if not (PORT / 'out' / exe).is_file():
            raise SystemExit(f'out/{exe} not found: build first (bash build.sh).')
    out = (a.out or PORT / 'dist').resolve()
    release = release_name()
    shipped = []

    def ship(source, name):
        copy(source, out / name)
        shipped.append(name)

    programs = {exe: PORT / 'out' / exe for exe in EXES}
    for name, source in programs.items():
        if a.debug:
            ship(source, name)
        else:
            strip(source, out / name)
            shipped.append(name)
        set_rpath(out / name, '$ORIGIN/lib')
    libs = so_closure(list(programs.values()))
    wanted = {lib.name for lib in libs}
    for old in (out / 'lib').glob('*') if (out / 'lib').is_dir() else ():
        if old.name not in wanted:
            old.unlink()
    for lib in libs:
        target = out / 'lib' / lib.name
        if a.debug:
            copy(lib, target)
        else:
            strip(lib, target, '--strip-unneeded')
        set_rpath(target, '$ORIGIN')
        shipped.append(f'lib/{lib.name}')
    (out / 'tempest.png').write_bytes(ico_png(ICON))
    shipped.append('tempest.png')
    prepare = out / 'prepare'
    python = prepare / 'python'
    for item in prepare.iterdir() if prepare.is_dir() else ():
        if item != python:
            shutil.rmtree(item) if item.is_dir() else item.unlink()
    for name in PREPARE_FILES:
        ship(PORT / name, f'prepare/{name}')
    if python.is_dir():
        shipped += [str(f.relative_to(out)).replace('\\', '/') for f in sorted(python.rglob('*')) if f.is_file()]

    (out / 'release.txt').write_bytes(f'{release}\n'.encode())
    (out / 'README.txt').write_bytes(README.format(release=release).encode())
    ship(PORT / 'LICENSE', 'LICENSE.txt')
    ship(PORT / 'gpu/third_party/fonts/orbitron/OFL.txt', 'Orbitron-OFL.txt')
    # RecompLauncher's profile (setup, start and settings), next to the exes as it expects.
    ship(PORT / 'recomplauncher.json', 'recomplauncher.json')
    shipped += ['release.txt', 'README.txt']
    size = sum((out / name).stat().st_size for name in shipped)
    glibc = min_glibc([out / name for name in shipped if name in programs or name.startswith('lib/')])
    print(f'{out}: {release}{" (debug)" if a.debug else ""}, {len(libs)} libraries, {len(shipped)} files, '
          f'{size / 2**20:.0f} MiB, needs glibc {glibc or "?"}')
    if a.zip:
        # tar keeps the exec bits a zip would lose.
        archive = PORT / 'out' / f'{release}-linux{"-debug" if a.debug else ""}.tar.gz'
        with tarfile.open(archive, 'w:gz', compresslevel=9) as t:
            for name in sorted(shipped):
                t.add(out / name, f'{release}/{name}')
        print(f'{archive}: {archive.stat().st_size / 2**20:.0f} MiB')
        if not a.debug:
            symbols = PORT / 'out' / f'{release}-linux-symbols.tar.gz'
            with tarfile.open(symbols, 'w:gz', compresslevel=9) as t:
                for exe in EXES:
                    t.add(PORT / 'out' / exe, exe)
            print(f'{symbols}: {symbols.stat().st_size / 2**20:.0f} MiB (not for players)')
    elif a.zip:
        archive = PORT / 'out' / f'{release}{"-debug" if a.debug else ""}.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(shipped):
                z.write(out / name, f'{release}/{name}')
        print(f'{archive}: {archive.stat().st_size / 2**20:.0f} MiB')
        if not a.debug:
            symbols = PORT / 'out' / f'{release}-symbols.zip'
            with zipfile.ZipFile(symbols, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for exe in EXES:
                    z.write(PORT / 'out' / exe, exe)
            print(f'{symbols}: {symbols.stat().st_size / 2**20:.0f} MiB (not for players)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

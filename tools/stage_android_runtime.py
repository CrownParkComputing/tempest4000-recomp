#!/usr/bin/env python3
"""Stage a private ARM64/glibc development runtime for a Tempest execution probe.

Android-compatible glibc syscall fallbacks follow Bachata's package-runtime.mjs at
be6bc2e9c60799e071dd2fafa6216e8d80ec619c. Only copied executable ELF segments are patched;
system libraries are never modified. The manifest records original and resulting hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

SYSCALLS = {
    'set_robust_list': (0xd2800c68, 0xd2800000),
    'clone3': (0xd2803668, 0x928004a0),
    'faccessat2': (0xd28036e8, 0x928004a0),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def executable_segments(data):
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('runtime inputs must be little-endian ELF64 AArch64 files')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 54)
    for i in range(phnum):
        kind, flags, offset, _, _, size = struct.unpack_from('<IIQQQQ', data, phoff + i * phsize)
        if offset > len(data) or size > len(data) - offset:
            raise ValueError('ELF segment exceeds its file')
        if kind == 1 and flags & 1:
            yield offset, offset + size


def patch_glibc(data, name):
    counts = {}
    for label, (mov, replacement) in SYSCALLS.items():
        count = 0
        for begin, end in executable_segments(data):
            for offset in range(begin, end - 3, 4):
                if struct.unpack_from('<I', data, offset)[0] != mov:
                    continue
                for svc in range(offset + 4, min(offset + 36, end - 3), 4):
                    if struct.unpack_from('<I', data, svc)[0] == 0xd4000001:
                        struct.pack_into('<I', data, svc, replacement)
                        count += 1
                        break
        counts[label] = count
    expected = {'set_robust_list': 1, 'clone3': 0, 'faccessat2': 0} if name.startswith('ld-') else {
        'set_robust_list': 2, 'clone3': 1, 'faccessat2': 1}
    if counts != expected:
        raise ValueError(f'{name}: unexpected glibc syscall layout {counts}; expected {expected}')
    return counts


def needed(path):
    info = subprocess.run(['readelf', '-d', str(path)], check=True, capture_output=True, text=True)
    return re.findall(r'\(NEEDED\).*?\[(.*?)\]', info.stdout)


def stage(binary, output, library_dirs):
    output.mkdir(parents=True, exist_ok=True)
    pending = [(binary.name, binary)]
    loader = 'ld-linux-aarch64.so.1'
    pending.append((loader, next((p / loader for p in library_dirs if (p / loader).is_file()), None)))
    manifest = {}
    while pending:
        name, source = pending.pop(0)
        if name in manifest:
            continue
        if source is None:
            raise FileNotFoundError(f'ARM64 runtime library not found: {name}')
        data = bytearray(source.read_bytes())
        list(executable_segments(data))  # validate every library before copying it
        original = digest(data)
        patches = patch_glibc(data, name) if name in (loader, 'libc.so.6') else {}
        target = output / name
        if target.resolve() == source.resolve():
            raise ValueError('staging must not overwrite its source files')
        target.write_bytes(data)
        target.chmod(0o755)
        manifest[name] = {'source_sha256': original, 'sha256': digest(data), 'patches': patches}
        for dependency in needed(source):
            if dependency not in manifest:
                pending.append((dependency, next((p / dependency for p in library_dirs
                                                  if (p / dependency).is_file()), None)))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Staged {len(manifest)} ARM64 binaries/libraries in {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--lib-dir', action='append', type=Path)
    args = parser.parse_args()
    stage(args.binary, args.out, args.lib_dir or [Path('/usr/aarch64-linux-gnu/lib'),
                                                 Path('/usr/lib/aarch64-linux-gnu')])


if __name__ == '__main__':
    main()

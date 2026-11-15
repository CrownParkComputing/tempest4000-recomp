#!/usr/bin/env python3
"""Stage a user-supplied ARM64 Mesa Turnip ZIP, recording its exact provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as package:
        metadata = json.loads(package.read('meta.json'))
        library = metadata['libraryName']
        if library != 'libvulkan_freedreno.so':
            raise SystemExit('This build requires a Mesa Turnip package')
        data = package.read(library)
    if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\0':
        raise SystemExit('Turnip must be a little-endian ARM64 ELF library')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / library).write_bytes(data)
    (args.output / 'driver-source.json').write_text(json.dumps({
        'archive': args.archive.name, 'sha256': hashlib.sha256(args.archive.read_bytes()).hexdigest(),
        'library_sha256': hashlib.sha256(data).hexdigest(), 'metadata': metadata}, indent=2) + '\n')


if __name__ == '__main__':
    main()

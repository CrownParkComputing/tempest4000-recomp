#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract a PS4 .pkg into the game folder the runner reads (eboot.bin, sce_sys, sce_module, ...).

A .pkg is an NPX package: a header, a table of entries and a PFS image whose game data is
encrypted (AES-128-XTS). Inside that image sits the game's own PFS, compressed (PFSC), and inside
that the files themselves. This walks the three layers and writes the files out.

The bulk of the work is the XTS decryption of the PFS image, which is far too slow in Python, so
the setup program does it (setup/pkgdec.cpp) and this calls it: --xts-tool names the program.

No third-party modules: only the standard library. The two RSA keysets below are the published
ones of the fake-pkg scheme, from Antti/orbis (MIT OR Apache-2.0, Copyright (c) 2026 videobitva):
the keys every extractor uses, so a .pkg needs no console and no key of the player's own.
"""
import argparse
import hashlib
import hmac
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

def _keyset(modulus, exponent):
    """An RSA keyset: the modulus and the private exponent, in hex."""
    return int(modulus, 16), int(exponent, 16)


# The RSA keysets of the fake-pkg scheme (the private keys are public: they are what the console
# and every extractor use). From Antti/orbis, orbis-pkg/src/keys.rs (MIT OR Apache-2.0,
# Copyright (c) 2026 videobitva): pkg_key3 unwraps the package's slot-3 key, fake_pfs_key the
# key of the PFS image.
PKG_KEY3 = _keyset(
    'd212fc335f6ddb831609628b0356273782d477853529392d526b8c4c8cfb06c1'
    '845be7d4f7bcd24e6245cd2abbd77776453655273fb3f5f98eda4befaa59aeb3'
    '9bea5498d206326a58312ae0d44f90b50a7decf43a9c52672d99318e0c43e682'
    'fe0746e12e50d41f2d2f7ed908ba06b3bf2e203f4e3ffe44ffaa504357916994'
    '49158282e40f4c8d9d2cc95b1d64bf888bd4c594e76547841ee57910fb989347'
    'b97d8512a640982cf792bc951932ede890560d65c1aa78c62e54fd5f54a1f67e'
    'e5e05f61c120b4b9b4330870e4df8956ed012946775f8cb8a9f51e2eb3b9bfe0'
    '09b78d28d4a6c3b81e1f07ebb4120b95b88530fddc3913d07cdc8fedf9c9a3c1',
    '32d903908fbdb08f572b285e0b8db3ea5cd17ea890888cdd6a80bbb1dfc1f70d'
    'aa32f0b77ccb88800e8b64b0be4cd60e9b8c1e2a64e1f35cd77601415e935c94'
    'fedd4662c31b5ae2a0bc2debc3980aa7b7856970682b644ab31fcc7ddc7c26f4'
    '77f65cf2ae5a442dd3ab16620419bafb90ffe23050896ecb56b2ebc09116925e'
    '308eaec7945dfd35e120f8ad3ebc08bfc036749fd5bb5208fd0666f37ab304f4'
    '75295de95faa1030b20f5a1ac12ab3fecb21ad80ec8f20091cdbc55894c29cc6'
    'ce82653e5790bca98b06b4f072f677df9864f1ecfe372dbcae8c08811fc3c989'
    '1ac742824b2edc8e8d73ceb1cc01d90870873c4408ec498f815ae240ff77fc0d')

FAKE_PFS_KEY = _keyset(
    'c6cf71e7e59af0d12a2c458bf92a0ec143058bc37117801dcd497dde359d259b'
    'a0d7a0f27d6c087eaa5502682b23c644b84418eb56cf16a24803c9e74f87eb3d'
    '30c31588bf20e79dff770cde1d241e63a94f8abf5bbe601968333bfced9f474e'
    '5ff8eacb3d00bd6701f92c6dc6ac1364e76714f3dc52696ab9832c4230131bb2'
    'd8a5020d79ed96b10df8cc0cdf81954f035809570e80692efeff5277ea7528a8'
    'fbc9bebf9fbbb7798e1805e180bd50349481d353c269a2d24ccf6cf4572c104a'
    '3ffb22fd8b97e2c95ba62bcdd61b6bdb687f4bc2a05034c005e58def2467ff93'
    '40cf2d62a2a050b1f13aa83dfd80d1f9b80522afc8354590588ee33a7cbd3e27',
    '7f76cd0ee2d4de051cc6d9a80e8dfa7bca1eaa271a40f8f1228735dddbfdeef8'
    'c2bcbd01fb8be23e63b2b1225c56496e11be07440b9a2666d1492c8fd31bcfa4'
    'a1b8d1fba49ed2212883098af6a00ba3d60f9b6368ccbc0c4e145b27a4a9f42b'
    'b9b87bc0e651ad1d77d46bb9ce20d126667e5e9ea2e96b90f373b8528f441103'
    '0c1397393d132258d5438249da6e7ca1c58ca5b009e0ce3ddff49d3c9715e26a'
    'c72b3c509323dbba4a226644ac78bb0e1a2743b57167aff4ab48469373d042ab'
    '9363e56c9ade5024c0237d99793f2207e0c148561bdf830912b42d456bc9c068'
    '85999079961ad7f54d1f3783404aec3937a680927dc580c7d66ffe8a7989c6b1')


# The entries of a package we need.
ENTRY_KEYS = 0x10  # the encrypted keys of the package
ENTRY_PFS_KEY = 0x20  # the encrypted key of the PFS image
ENTRY_NAMES = 0x200  # the names of the files the package holds


class PkgError(Exception):
    """The .pkg is not one this can read."""


# --- AES-128 (FIPS-197) ---------------------------------------------------------------------
# Only a few hundred bytes of a package are encrypted this way (the keys), so a plain
# implementation is fast enough; the game's data is decrypted by the setup program instead.

def _sbox():
    """The AES S-box, from its definition."""
    box = [0] * 256
    p = q = 1
    while True:
        p = (p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= (q << 1) & 0xFF
        q ^= (q << 2) & 0xFF
        q ^= (q << 4) & 0xFF
        if q & 0x80:
            q ^= 0x09
        q &= 0xFF
        value = q
        for shift in (1, 2, 3, 4):
            value ^= ((q << shift) | (q >> (8 - shift))) & 0xFF
        box[p] = (value ^ 0x63) & 0xFF
        if p == 1:
            break
    box[0] = 0x63
    return box


SBOX = _sbox()
INVERSE = [0] * 256
for _i, _v in enumerate(SBOX):
    INVERSE[_v] = _i
RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def _gmul(a, b):
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        b >>= 1
        a = _xtime(a)
    return result


class Aes128:
    """AES-128: encrypting and decrypting one block, and a CBC stream."""

    def __init__(self, key):
        if len(key) != 16:
            raise PkgError('the AES key is not 16 bytes')
        words = [list(key[i * 4:i * 4 + 4]) for i in range(4)]
        for i in range(4, 44):
            temp = list(words[i - 1])
            if i % 4 == 0:
                temp = temp[1:] + temp[:1]
                temp = [SBOX[b] for b in temp]
                temp[0] ^= RCON[i // 4 - 1]
            words.append([words[i - 4][j] ^ temp[j] for j in range(4)])
        self.words = words

    def _add_round_key(self, state, rnd):
        for column in range(4):
            word = self.words[rnd * 4 + column]
            for row in range(4):
                state[row][column] ^= word[row]

    def encrypt_block(self, block):
        state = [[block[row + 4 * column] for column in range(4)] for row in range(4)]
        self._add_round_key(state, 0)
        for rnd in range(1, 11):
            for row in range(4):
                for column in range(4):
                    state[row][column] = SBOX[state[row][column]]
            for row in range(1, 4):
                state[row] = state[row][row:] + state[row][:row]
            if rnd != 10:
                for column in range(4):
                    a = [state[row][column] for row in range(4)]
                    state[0][column] = _gmul(a[0], 2) ^ _gmul(a[1], 3) ^ a[2] ^ a[3]
                    state[1][column] = a[0] ^ _gmul(a[1], 2) ^ _gmul(a[2], 3) ^ a[3]
                    state[2][column] = a[0] ^ a[1] ^ _gmul(a[2], 2) ^ _gmul(a[3], 3)
                    state[3][column] = _gmul(a[0], 3) ^ a[1] ^ a[2] ^ _gmul(a[3], 2)
            self._add_round_key(state, rnd)
        return bytes(state[row][column] for column in range(4) for row in range(4))

    def decrypt_block(self, block):
        state = [[block[row + 4 * column] for column in range(4)] for row in range(4)]
        self._add_round_key(state, 10)
        for rnd in range(9, -1, -1):
            for row in range(1, 4):
                state[row] = state[row][-row:] + state[row][:-row]
            for row in range(4):
                for column in range(4):
                    state[row][column] = INVERSE[state[row][column]]
            self._add_round_key(state, rnd)
            if rnd != 0:
                for column in range(4):
                    a = [state[row][column] for row in range(4)]
                    state[0][column] = (_gmul(a[0], 14) ^ _gmul(a[1], 11) ^ _gmul(a[2], 13) ^ _gmul(a[3], 9))
                    state[1][column] = (_gmul(a[0], 9) ^ _gmul(a[1], 14) ^ _gmul(a[2], 11) ^ _gmul(a[3], 13))
                    state[2][column] = (_gmul(a[0], 13) ^ _gmul(a[1], 9) ^ _gmul(a[2], 14) ^ _gmul(a[3], 11))
                    state[3][column] = (_gmul(a[0], 11) ^ _gmul(a[1], 13) ^ _gmul(a[2], 9) ^ _gmul(a[3], 14))
        return bytes(state[row][column] for column in range(4) for row in range(4))

    def decrypt_cbc(self, data, iv):
        """The decrypted stream; the data is a multiple of 16 bytes."""
        if len(data) % 16:
            raise PkgError('the encrypted data is not a multiple of 16 bytes')
        out = bytearray()
        previous = iv
        for at in range(0, len(data), 16):
            block = data[at:at + 16]
            out += bytes(a ^ b for a, b in zip(self.decrypt_block(block), previous))
            previous = block
        return bytes(out)


def rsa_decrypt(keyset, ciphertext):
    """The PKCS#1 v1.5 decryption of a block with an RSA private key."""
    n, d = keyset
    value = pow(int.from_bytes(ciphertext, 'big'), d, n)
    padded = value.to_bytes((n.bit_length() + 7) // 8, 'big')
    if len(padded) < 11 or padded[0] != 0 or padded[1] != 2:
        raise PkgError('the package is encrypted with keys this does not have')
    end = padded.index(b'\0', 2)
    if end < 10:
        raise PkgError('the package is encrypted with keys this does not have')
    return padded[end + 1:]


# --- the package ----------------------------------------------------------------------------

class Pkg:
    """A PS4 .pkg: its header, its entries and the PFS image in it."""

    def __init__(self, path):
        self.path = Path(path)
        try:
            self.data = self.path.read_bytes()
        except OSError as error:
            raise PkgError(f'cannot read {self.path}: {error}') from error
        if len(self.data) < 0x1000:
            raise PkgError(f'{self.path.name} is too small to be a .pkg')
        if self.data[:4] != b'\x7fCNT':
            raise PkgError(f'{self.path.name} is not a PS4 .pkg')
        self.table_offset, self.table_size = self._be32(0x18), self._be32(0x1C)
        self.entry_count = self._be32(0x10)
        self.pfs_offset, self.pfs_size = self._be64(0x410), self._be64(0x418)
        self._entry_key = None
        self.entries = {}
        for index in range(self.entry_count):
            at = self.table_offset + index * 32
            if at + 32 > len(self.data):
                raise PkgError('the package table is truncated')
            entry_id, name_offset, flags1, flags2, offset, size = struct.unpack_from('>6I', self.data, at)
            self.entries.setdefault(entry_id, []).append(
                dict(raw=self.data[at:at + 32], flags=flags1, key_index=(flags2 >> 12) & 0xF,
                     offset=offset, size=size, name_offset=name_offset))

    def _be32(self, offset):
        return struct.unpack_from('>I', self.data, offset)[0]

    def _be64(self, offset):
        return struct.unpack_from('>Q', self.data, offset)[0]

    def _entry(self, entry_id):
        entries = self.entries.get(entry_id)
        if not entries:
            raise PkgError(f'the package has no entry {entry_id:#x}')
        return entries[0]

    def entry_data(self, entry, key=None):
        """The data of an entry, decrypted when the entry is encrypted (the key is needed then)."""
        if not entry['flags'] & 0x80000000:
            return self.data[entry['offset']:entry['offset'] + entry['size']]
        if key is None:
            key = self.entry_key()
        secret = hashlib.sha256(entry['raw'] + key).digest()
        cipher = Aes128(secret[16:32])
        padded = (entry['size'] + 15) & ~15
        raw = self.data[entry['offset']:entry['offset'] + padded]
        if len(raw) < padded:
            raise PkgError('the package ends inside an entry')
        return cipher.decrypt_cbc(raw, secret[:16])[:entry['size']]

    def named_entries(self):
        """The files the package names itself: (name, entry) for each of them."""
        table = self._entry(ENTRY_NAMES)
        names = self.data[table['offset']:table['offset'] + table['size']]
        found = []
        for entries in self.entries.values():
            for entry in entries:
                at = entry['name_offset']
                if at >= len(names):
                    continue
                end = names.find(b'\0', at)
                if end < 0:
                    continue
                name = names[at:end]
                if name:
                    found.append((name, entry))
        return found

    def entry_key(self):
        """The key of the package's own encrypted entries (the slot 3 key, unwrapped)."""
        if self._entry_key is None:
            keys = self._entry(ENTRY_KEYS)
            raw = self.data[keys['offset']:keys['offset'] + keys['size']]
            if len(raw) < 32 + 7 * 32 + 7 * 256:
                raise PkgError('the package has no usable key entry')
            slot3 = raw[32 + 7 * 32 + 3 * 256:32 + 7 * 32 + 4 * 256]
            try:
                self._entry_key = rsa_decrypt(PKG_KEY3, slot3)
            except PkgError:
                raise PkgError('this .pkg needs the keys of a console, not the ones of a fake package') from None
        return self._entry_key

    def pfs_keys(self):
        """The keys of the PFS image: (data key, tweak key), the offset of the image, its size and
        the first sector that is encrypted."""
        image_key = self._entry(ENTRY_PFS_KEY)
        decrypted = self.entry_data(image_key, self.entry_key())
        try:
            pfs_key = rsa_decrypt(FAKE_PFS_KEY, decrypted[:256])
        except PkgError:
            raise PkgError('this .pkg needs the keys of a console, not the ones of a fake package') from None
        header = self.data[self.pfs_offset:self.pfs_offset + 0x380]
        if len(header) < 0x380:
            raise PkgError('the package has no game data')
        seed = header[0x370:0x380]
        secret = hmac.new(pfs_key, b'\x01\x00\x00\x00' + seed, hashlib.sha256).digest()
        return secret[16:32], secret[:16], self.pfs_offset, self.pfs_size


# --- the PFS image --------------------------------------------------------------------------

class Image:
    """Bytes of a PFS image, read at an offset."""

    def read_at(self, offset, size):
        raise NotImplementedError


class FileImage(Image):
    """A file, read at an offset."""

    def __init__(self, path):
        self.file = open(path, 'rb')

    def read_at(self, offset, size):
        self.file.seek(offset)
        data = self.file.read(size)
        return data


class Pfs:
    """A PFS filesystem: its header, its inodes and its directories."""

    HEADER_SIZE = 0x380

    def __init__(self, image):
        self.image = image
        header = image.read_at(0, self.HEADER_SIZE)
        version, fmt = struct.unpack_from('<QQ', header, 0)
        if version != 1 or fmt != 20130315:
            raise PkgError('the game data is not a PFS image')
        self.mode = struct.unpack_from('<H', header, 0x1C)[0]
        self.block_size = struct.unpack_from('<I', header, 0x20)[0]
        self.inode_count = struct.unpack_from('<Q', header, 0x30)[0]
        self.inode_block_count = struct.unpack_from('<Q', header, 0x40)[0]
        self.superroot = struct.unpack_from('<Q', header, 0x48)[0]
        if not self.block_size or self.block_size & (self.block_size - 1):
            raise PkgError('the game data has a bad block size')
        if self.mode & 2:
            raise PkgError('the game data is in a format this does not read')
        self.signed = bool(self.mode & 1)
        self.inode_size = 712 if self.signed else 168
        self.inodes = self._read_inodes()

    def _read_inodes(self):
        table = self.image.read_at(self.block_size, self.inode_size * self.inode_count)
        inodes = []
        for index in range(self.inode_count):
            raw = table[index * self.inode_size:(index + 1) * self.inode_size]
            if len(raw) < self.inode_size:
                raise PkgError('the game data has a truncated inode table')
            mode = struct.unpack_from('<H', raw, 0)[0]
            flags = struct.unpack_from('<I', raw, 4)[0]
            size = struct.unpack_from('<Q', raw, 8)[0]
            blocks = struct.unpack_from('<I', raw, 0x60)[0]
            direct, indirect = [], []
            if self.signed:
                for i in range(12):
                    direct.append(struct.unpack_from('<I', raw, 0x64 + i * 36 + 32)[0])
                for i in range(5):
                    indirect.append(struct.unpack_from('<I', raw, 0x64 + 432 + i * 36 + 32)[0])
            else:
                for i in range(12):
                    direct.append(struct.unpack_from('<I', raw, 0x64 + i * 4)[0])
                for i in range(5):
                    indirect.append(struct.unpack_from('<I', raw, 0x64 + 48 + i * 4)[0])
            inodes.append(dict(mode=mode, flags=flags, size=size, blocks=blocks,
                               direct=direct, indirect=indirect))
        return inodes

    def _block_map(self, inode):
        """The blocks of an inode, in order."""
        count = inode['blocks']
        if count == 0:
            return []
        if inode['direct'][1] == 0xFFFFFFFF:
            return list(range(inode['direct'][0], inode['direct'][0] + count))
        blocks = list(inode['direct'][:12])
        if len(blocks) >= count:
            return blocks[:count]
        entry_size, value_at = (36, 32) if self.signed else (4, 0)

        def entries(block):
            """The numbers an index block holds."""
            data = self.image.read_at(block * self.block_size, self.block_size)
            for at in range(0, len(data) - entry_size + 1, entry_size):
                yield struct.unpack_from('<I', data, at + value_at)[0]

        # The first index block holds the blocks of the file; the second holds the index blocks
        # of the rest of it (the file is too big for the first one alone).
        for value in entries(inode['indirect'][0]):
            blocks.append(value)
            if len(blocks) >= count:
                return blocks[:count]
        for index in entries(inode['indirect'][1]):
            for value in entries(index):
                blocks.append(value)
                if len(blocks) >= count:
                    return blocks[:count]
        raise PkgError('the game data has a file it cannot map')

    def file_data(self, inode):
        """The bytes of a file: its blocks, in order."""
        out = bytearray()
        for block in self._block_map(inode):
            out += self.image.read_at(block * self.block_size, self.block_size)
        return bytes(out[:inode['size']])

    def directory(self, inode):
        """The entries of a directory: (name, inode number, kind)."""
        data = self.file_data(inode)
        entries = []
        at = 0
        while at + 16 <= len(data):
            number, kind, name_length, entry_size = struct.unpack_from('<4I', data, at)
            if entry_size < 16 or at + entry_size > len(data):
                break
            name = data[at + 16:at + 16 + name_length]
            entries.append((name, number, kind))
            at += entry_size
        return entries

    def find(self, inode, name):
        """The inode number of an entry of a directory."""
        for entry_name, number, _ in self.directory(inode):
            if entry_name == name:
                return number
        return None

    def root(self):
        """The inode the game's files are in: the 'uroot' of the superroot, or the superroot."""
        uroot = self.find(self.inodes[self.superroot], b'uroot')
        return self.inodes[uroot] if uroot is not None else self.inodes[self.superroot]


class Pfsc(Image):
    """A PFSC container: a PFS image whose blocks are deflated, read at an offset."""

    def __init__(self, source):
        self.source = source
        header = source.read_at(0, 48)
        if header[:4] != b'PFSC':
            raise PkgError('the game data is not compressed the way this expects')
        self.block_size = struct.unpack_from('<I', header, 0x0C)[0]
        self.original_size = struct.unpack_from('<Q', header, 0x10)[0]
        self.offsets_at = struct.unpack_from('<Q', header, 0x18)[0]
        self.length = struct.unpack_from('<Q', header, 0x28)[0]
        count = (self.length + self.original_size - 1) // self.original_size + 1
        table = source.read_at(self.offsets_at, count * 8)
        self.offsets = list(struct.unpack_from(f'<{count}Q', table))

    def _block(self, number):
        """One decompressed block."""
        start, end = self.offsets[number], self.offsets[number + 1]
        size = end - start
        if size > self.original_size:
            return bytes(self.original_size)  # a sparse block
        data = self.source.read_at(start, size)
        if size == self.original_size:
            return data
        try:
            return zlib.decompress(data)
        except zlib.error:
            return zlib.decompress(data, -15)  # a block without the zlib header

    def read_at(self, offset, size):
        out = bytearray()
        while len(out) < size and offset < self.length:
            number = offset // self.original_size
            inside = offset % self.original_size
            block = self._block(number)
            take = min(len(block) - inside, size - len(out), self.length - offset)
            out += block[inside:inside + take]
            offset += take
        return bytes(out)


# --- the extraction -------------------------------------------------------------------------

def decrypt_image(pkg, out, xts_tool, work):
    """The decrypted PFS image of the package, in a file of its own."""
    data_key, tweak_key, offset, size = pkg.pfs_keys()
    header = pkg.data[offset:offset + 0x380]
    block_size = struct.unpack_from('<I', header, 0x20)[0]
    first_encrypted = block_size // 0x1000
    command = [str(xts_tool), '--pkg-decrypt', str(pkg.path), str(offset), str(size), str(work),
               data_key.hex(), tweak_key.hex(), str(first_encrypted)]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        message = (result.stderr or result.stdout or '').strip()
        raise PkgError(f'decrypting the game data failed: {message}' if message else
                       'decrypting the game data failed')
    return work


def extract(pkg_path, out_path, xts_tool):
    """The files of a .pkg, written into out_path."""
    pkg = Pkg(pkg_path)
    out_path = Path(out_path)
    out_path.mkdir(parents=True, exist_ok=True)
    # The decrypted image is big (the size of the game), so it goes next to the files.
    handle, work = tempfile.mkstemp(prefix='tempest-pfs-', suffix='.tmp', dir=str(out_path.parent))
    os.close(handle)
    try:
        decrypt_image(pkg, out_path, xts_tool, work)
        outer = Pfs(FileImage(work))
        # The game's own filesystem is a file of the outer one, compressed.
        inner_source = None
        for name, number, kind in outer.directory(outer.root()):
            if name == b'pfs_image.dat':
                inner_source = outer.file_data(outer.inodes[number])
                break
        if inner_source is None:
            raise PkgError('the package holds no game data')
        inner = Pfs(Pfsc(BytesImage(inner_source)))
        written = extract_files(inner, inner.root(), out_path)
        # The files the package holds itself (param.sfo, the icons, the change info, the
        # trophies) are the game's sce_sys; the PFS image holds the rest.
        written += write_named_entries(pkg, out_path / 'sce_sys')
        print(f'{len(written)} files, {sum(p.stat().st_size for p in written):,} bytes')
    finally:
        try:
            os.unlink(work)
        except OSError:
            pass
    return out_path


class BytesImage(Image):
    """Bytes in memory, read at an offset."""

    def __init__(self, data):
        self.data = data

    def read_at(self, offset, size):
        return self.data[offset:offset + size]


def extract_files(pfs, inode, folder):
    """The files of a directory of a PFS, written into a folder. The list of the files written."""
    written = []
    for name, number, kind in pfs.directory(inode):
        if kind in (4, 5) or not name or b'/' in name or name in (b'.', b'..'):
            continue
        entry = pfs.inodes[number]
        target = folder / name.decode('utf-8', 'replace')
        if kind == 3 or entry['mode'] & 0x4000:
            target.mkdir(parents=True, exist_ok=True)
            written += extract_files(pfs, entry, target)
        else:
            data = pfs.file_data(entry)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            written.append(target)
    return written


def write_named_entries(pkg, folder):
    """The files the package holds itself, written into a folder (the game's sce_sys)."""
    written = []
    for name, entry in pkg.named_entries():
        target = folder / name.decode('utf-8', 'replace')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(pkg.entry_data(entry))
        written.append(target)
    return written


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pkg', type=Path, help='the .pkg to extract')
    parser.add_argument('--out', type=Path, required=True, help='the folder to write the files into')
    parser.add_argument('--xts-tool', type=Path, required=True,
                        help='the program that decrypts the game data (the setup)')
    args = parser.parse_args()
    sys.stdout.reconfigure(errors='replace')
    try:
        extract(args.pkg, args.out, args.xts_tool)
    except (PkgError, OSError) as error:
        parser.exit(1, f'pkg_extract failed: {error}\n')

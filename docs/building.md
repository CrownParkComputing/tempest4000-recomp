# Building from source

Everything builds on Linux x86-64 with clang. The same steps run in CI
(`.github/workflows/build.yml`), so a green run there is a working recipe. This fork is Linux only;
the upstream project also builds for Windows with MSYS2's CLANG64 toolchain.

## 1. Toolchain

clang, CMake, Ninja, pkg-config, make, curl and Python 3.10 or newer, plus the Vulkan, SDL3, fmt,
Boost, robin-map, xxHash, zlib, Zydis, nasm and SPIRV development packages. On Ubuntu 24.04 (also
in WSL), `.github/scripts/ci-deps.sh` installs them as root, building Vulkan-Headers, VMA and SDL3,
which 24.04 lacks or ships too old. On Arch they are all in the repositories.

## 2. Source

Clone the repository with its submodules (imgui, magic_enum, miniz and xbyak, under
`gpu/third_party`):

```bash
git clone --recurse-submodules <repository URL>
```

`build.sh` fetches the submodules itself if the clone was made without them. The runner links a
small static FFmpeg (9.0.2; `tools/build_ffmpeg.sh`): only the H.264, AAC and PNG decoders, the MP4
demuxer and the file protocol, which is all Tempest's movies and the save icons need. A build of it
is committed in `prebuilt/ffmpeg-linux.tar.xz`, and the first build unpacks it into `out/ffmpeg`. If that archive was made
with another version or options than the script names, FFmpeg's source is downloaded (checked
against its SHA-256) and built instead, which takes a few minutes; `nasm` gives it its x86 assembly
(without it, decoding is slower). `TEMPEST_FFMPEG_PREBUILT=0` always builds from source.

After changing FFmpeg's version or options, rewrite the archive and commit it with
`bash tools/pack_ffmpeg_linux.sh`, which builds in an Ubuntu 24.04 container (Docker) so the
libraries need no newer glibc than the release.

## 3. Build

At the repository root:

```bash
bash build.sh
```

It writes `out/tempest` (loader, runtime and GPU library in one binary, linked without PIE: host
pointers handed to the game must stay below 1 TiB) and `out/tempest-setup`. Both are RelWithDebInfo
builds and carry their debug info (about 120 MB of the game binary) for debuggers and
`llvm-addr2line`. A failed build prints its last lines; the full log is
`out/gpu-build.log`.

Tests:

```bash
bash build.sh --test
```

```bash
cd tests && python -m unittest discover -p "test_*.py"
```

`--test` builds and runs the runtime's C tests and every `*-test` target of `gpu/CMakeLists.txt`
(the renderer's C++ tests and the rest of the runtime's), from the repository root; a failed one
prints the end of its log, `out/<name>-test.log`. Tests that need the game dump, a Vulkan device or
`iced-x86` skip themselves when those are missing.

### Convenience scripts

For local work, `build_linux.sh` wraps the steps above:

```bash
./build_linux.sh --test --run --game <pkg or dump>
```

Without options it builds. `--test` also runs the Python tests, `--run` starts the game for
`--timeout` seconds (20 by default; `TEMPEST_GAME_DIR` names the dump when `--game` is left out)
and prints its first `STOP:` or `Fault:` line, with the full log in `out/boot.log`. `--package`
writes the release (section 5), and `--clean` removes `out/` first. It picks `clang-18`-style
compiler names when there is no plain `clang`.

In WSL the game runs on Mesa's software Vulkan (llvmpipe): slow, but enough for a boot check.

## 4. Run from the source tree

Point `run.py` at your decrypted dump (CUSA00038, version 1.10: `eboot.bin`, `sce_module`,
`sce_sys`, `audio.pck`, `stream.dat`) with `--game DIR` or `TEMPEST_GAME_DIR`:

```bash
python run.py --game <dump> --timeout 20
```

`run.py` prepares `out/game/` from the dump when needed and starts the game.
[development.md](development.md) covers its other commands.

## 5. Package

`python3 tools/package.py --zip` writes `out/<release>-linux.tar.gz` (also `-symbols`, and `-debug`
with `--debug`): the game, the setup, the libraries they need in `lib/` less the host's (glibc,
libstdc++, Vulkan, X11/Wayland, audio), `prepare/` for the setup, a README and the licence. The
setup runs the system's python3. It never contains game files.

`.github/scripts/smoke-test.sh <unpacked release>` checks it as root on a fresh distro (libraries,
the setup on non-dumps, the start from `paths.ini`); the `linux-smoke` CI job runs it on Ubuntu
24.04, Debian 13, Fedora 42 and Arch.

The folder (`dist/` by default, `--out DIR` otherwise) is also a working install: run its setup once
and it plays. Packaging into it again keeps what the setup wrote there (`game/`, `paths.ini`,
`settings.ini`, logs).

### Symbols for release crashes

A release `--zip` also writes `out/<release>-linux-symbols.tar.gz` with the unstripped programs of
that build; it is not for players. Resolve an offset in a player's `tempest.log` with the symbols of
the same release (named in their `release.txt`):

```bash
llvm-addr2line -fCe tempest 0x1234ab
```

## 6. CI and releases

Pushing a tag `v*` runs `.github/workflows/build.yml` (so do pull requests and a manual run from the
Actions tab; pushes to branches do not): the `linux` job builds and runs both test suites and the
package on `ubuntu-24.04`, and `linux-smoke` checks the unpacked release on four distros. The
package is the run's artifact (Actions tab, kept 90 days). A tag run that passes also publishes the
tarball as a GitHub release named after the tag.
The release's text is the tag's section of `CHANGELOG.md` (`## v0.4` up to the next `## `), then
the install instructions. Write the section first: a tag without one fails the `notes` job at once
and does not publish. `python tools/release_notes.py v0.4` prints what the release will say.

```bash
python tools/release_notes.py v0.1.0   # the section is there
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

# Tempest 4000 PC runner

Native Linux runner for the PS4 version of Tempest 4000 (CUSA09856, v1.02; Llamasoft's engine for
Atari, statically linked into eboot.bin, with Scream 7.23.5.0 driving NGS2 voices and its audio in
txk-NGS2.bnk as ATRAC9). The game's x86-64 code runs directly; PS4 system calls are reimplemented
in C and GNM rendering goes through shadPS4's video core on Vulkan.

This runner began as [CrownParkComputing/resogun-recomp](https://github.com/CrownParkComputing/resogun-recomp),
itself a fork of [rcostanza/resogun-recomp](https://github.com/rcostanza/resogun-recomp), whose
authors wrote the loader, the HLE runtime and the renderer. Their Resogun-specific code has been
renamed, not rewritten: expect its shape everywhere. See [docs/development.md](docs/development.md) for the
pipeline and [docs/building.md](docs/building.md) for the toolchain and packaging. No game files
are in the repository, and nothing assumes where the decrypted dump is: set `TEMPEST_GAME_DIR`
to it (or pass `--game DIR` to `run.py`). Tests that need it skip without it.

## Loop
- Build: `bash build.sh` (Linux; this fork is Linux only).
- Run: `python run.py --timeout 20`; read the first `STOP:` or `Fault:` line.
- Missing imports: `python run.py check` (full list in `out/game/missing-imports.txt`).
- Tests: `bash build.sh --test` (runtime C tests and every CMake `*-test`), then
  `cd tests && python -m unittest discover -p "test_*.py"`. `build_linux.sh --test --run` does
  the build, both suites and a boot check in one go.

## Conventions
- Names are Tempest's: identifiers, files, env variables (`TEMPEST_*`), log prefixes and
  patch markers in vendored code (`// tempest:`).
- C runtime imports resolve by symbol name: a function goes in a `src/runtime_*.c` export table
  (`{"name",function}`) and its NID in `src/import_names.inc`. Name NIDs with
  `python tools/name_imports.py out/game --write` (candidates in `tools/ps4_names.txt`).
- `libScePosix` imports are looked up under the `libkernel` key (runtime.c, `canonical_name`).
- POSIX variants return errno; `sce*` variants return SCE codes (0x8002xxxx, errno in the low
  16 bits). See `posix_result` in runtime_mutex.c.
- The loader builds with `-Wall -Wextra -Werror`. Match the surrounding terse C style and comment
  each function with the PS4 contract it follows.
- Guest-visible pointers must stay below 1 TiB (`runtime_low_alloc`).
- Keep only what this game needs: no extra menus or hacks for other games. The renderer
  is shadPS4's with added performance work (draw preparation, memos, async pipelines); behaviour
  changes there need a boot check (`python run.py --timeout 20`).
- Releases are built by `tools/package.py` and must work from the unpacked zip alone: no repo, no
  MSYS2, no Python of the player's own. Never package game files or anything built from a dump.
- This game ships no DLC; sceAppContent answers from the offline profile in runtime_content.c.
- This engine takes no command-line switches of its own: the eboot holds none of the strings
  Resogun's did, and passing one leaves it waiting in its intro video.
- Audio is ATRAC9 through NGS2 (src/runtime_ngs2.c) with LibAtrac9 in third_party/libatrac9.
  A voice must report TEMPEST_STATE_IN_USE or the engine never sends it waveform data, and
  sceNgs2GetWaveformFrameInfo must answer with real frame geometry or Scream refuses to load the bank.

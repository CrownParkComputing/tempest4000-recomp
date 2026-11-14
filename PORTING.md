# Tempest 4000 (CUSA09856) on this runner

What follows is measured, not estimated: every number came from running the tools in this
repository against a `.pkg` of Tempest 4000 version 1.02 (UP0182-CUSA09856_00, firmware 6.72) on
2026-11-14.

## Where it stands

| Step | State |
|---|---|
| Extract the `.pkg` | **works** — 58 files, 196,109,540 bytes, about 4 seconds, no changes needed |
| Parse the eboot | **works** — a fake-signed SELF with plaintext segments, same as Tempest's |
| Prepare the image | **works** — `Tempest 4000 \| entry=0xb5380 \| image=2,608,905 bytes` |
| Link the modules | **works** — libc, libSceFios2 and libSceS3DConversion (deferred); 120 native bindings |
| Boot | **no** — stops in libc init at `STOP: first unsupported PS4 import: rNhWz+lvOMU#libkernel` |

The game's entry point has not run yet, so nothing is drawn. Reproduce with:

```bash
python run.py check --any-build --game <extracted dump> --out out/tempest
```

Use `--any-build`: `VERIFIED_BUILDS` in `scripts/prepare.py` holds Tempest's eboot hashes, so any
other game's eboot is unverified by definition.

## What is missing

431 imported symbols, 3,782 relocations, 38 required modules. 439 imports fall to the host runtime
and **52 of them have no implementation**:

| Count | Library | Nature |
|---|---|---|
| 14 | `libkernel` | **blocking** — the first one stops libc init |
| 10 | `libSceNgs2` | audio; Tempest uses Wwise through `libSceAudioOut`, this game uses NGS2 |
| 5 | `libSceNpScore` | online leaderboards |
| 5 | `libSceSigninDialog` | PSN sign-in |
| 4 | `libSceAudio3d` | 3D audio |
| 3 | `libSceNetCtl` | network state |
| 3 | `libSceNpManager` | PSN accounts |
| 2 | `libSceNet` | sockets |
| 1 each | `libSceCamera`, `libSceNpTrophy`, `libSceRtc`, `libSceSaveData`, `libSceSysmodule`, `libSceVideoOut` | assorted |

Roughly 23 of the 52 are online and social (NpScore, NpManager, SigninDialog, NetCtl, Net,
NpTrophy) and can almost certainly return "unavailable" rather than be implemented. The real work
is the 14 `libkernel` entries, then the 10 `libSceNgs2` ones for audio.

`out/<image>/missing-imports.txt` lists all 52 with their NIDs; `tools/name_imports.py` resolves
names against `tools/ps4_names.txt`.

## shadPS4 already has almost all of it

Checked on 2026-11-14 against shadPS4 `main`, NID by NID, ignoring its `aerolib.inl` name table
(which lists every NID whether or not it is implemented):

**45 of the 52 have a real implementation in a shadPS4 library**, and this repository already
vendors that project's video core under `gpu/shadps4/` with its changes marked `// tempest:`, so
taking more of it follows a path that is already here. Same licence, GPL-2.0-or-later.

| Library | Missing here | In shadPS4 | Source |
|---|---|---|---|
| `libSceNgs2` | 10 | **10** | `core/libraries/ngs2/` (`ngs2.cpp` 20 KB, `ngs2_impl.cpp` 34 KB) |
| `libSceAudio3d` | 4 | **4** | `core/libraries/audio3d/` — needs OpenAL (`AL/al.h`) |
| `libSceNpScore` | 5 | **5** | `core/libraries/np/np_score/` |
| `libSceSigninDialog` | 5 | **5** | `core/libraries/signin_dialog/` |
| `libSceNetCtl` | 3 | **3** | `core/libraries/network/netctl.cpp` |
| `libSceNpManager` | 3 | **3** | `core/libraries/np/np_manager.cpp` |
| `libSceNet` | 2 | **2** | `core/libraries/net/net.cpp` |
| `libkernel` | 14 | **8** | `kernel/threads/{pthread,pthread_attr,mutex,event_flag}.cpp`, `kernel/{memory,time}.cpp` |
| `libSceCamera`, `libSceRtc`, `libSceSaveData`, `libSceNpTrophy`, `libSceSysmodule` | 1 each | **5** | their own folders |
| `libSceVideoOut` | 1 | 0 | see below |

**The blocker is covered**: `rNhWz+lvOMU#libkernel`, the import that stops libc initialization, is
implemented in `core/libraries/kernel/threads/pthread.cpp`.

### The 7 that are not implemented anywhere

shadPS4 marks these `STUB` in its own name table. Every one of them looks like a return-a-constant
stub rather than real work:

| NID | Name | Note |
|---|---|---|
| `pB-yGZ2nQ9o` | `_sceKernelSetThreadAtexitCount` | linker thread-atexit bookkeeping |
| `WhCc1w3EhSI` | `_sceKernelSetThreadAtexitReport` | same |
| `8OnWXlgQlvo` | `_sceKernelRtldThreadAtexitDecrement` | same |
| `Tz4RNUCBbGI` | `_sceKernelRtldThreadAtexitIncrement` | same |
| `py6L8jiVAN8` | `sceKernelGetSanitizerMallocReplaceExternal` | ASan hook; return null |
| `bnZxYgAFeA0` | `sceKernelGetSanitizerNewReplaceExternal` | ASan hook; return null |
| `lCTCOogRbk0` | `sceVideoOutRegisterStereoBuffers` | stereoscopic 3D; refusing it should cost only the 3D mode (the game ships `libSceS3DConversion`) |

So the import gap is **45 portable from shadPS4 plus 7 stubs**, not 52 functions to write. In
particular there is no need to convert or reimplement the game's audio: NGS2 is done.

### How imports are added here

A function goes in a `src/runtime_*.c` export table as `{"name", function}` with its NID in
`src/import_names.inc`; `libScePosix` imports are looked up under the `libkernel` key
(`runtime.c`, `canonical_name`). shadPS4's libraries are C++ against its own kernel objects, so
they are a reference and a source to adapt, not a drop-in: this runner has its own thread, mutex
and memory implementations in `src/runtime_*.c` that the adapted code has to sit on.

## What is Tempest-specific in here

This runner was written for one game, so expect to find assumptions:

- `scripts/prepare.py`: `VERIFIED_BUILDS` holds Tempest eboot hashes; `DLC_LOADER_SLOTS` in
  `scripts/link.py` holds GOT addresses of Tempest's eboot 01.10 (harmless here, nothing imports
  `dlcldr`).
- `setup/setup.cpp`: `missing_game_files` expects Tempest's `stream.dat` and `audio.pck`. Tempest
  ships `assets/` and loose `shader_*.sb` files instead, so the setup will reject the folder.
- `src/runtime_pad.c`: the intro automation presses Cross on Tempest's title screen when
  AvPlayer opens `title.mp4`.
- `src/runtime_ajm.c`: assumes Wwise ADPCM and Vorbis.
- Names, env variables and log prefixes are all `TEMPEST_*` / `tempest`.

## Worth knowing before investing

Tempest 4000 already has an official PC release (Steam and GOG). Unlike Tempest, which is
PlayStation-exclusive, anyone wanting it on a PC can buy it there. That does not make this port
pointless, but it is a different proposition from the Tempest one.

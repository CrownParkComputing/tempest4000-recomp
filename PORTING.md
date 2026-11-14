# Tempest 4000 (CUSA09856) on this runner

What follows is measured, not estimated: every number came from running the tools in this
repository against a `.pkg` of Tempest 4000 version 1.02 (UP0182-CUSA09856_00, firmware 6.72) on
2026-11-14.

## Where it stands

| Step | State |
|---|---|
| Extract the `.pkg` | **works** — 58 files, 196,109,540 bytes, about 4 seconds, no changes needed |
| Parse the eboot | **works** — a fake-signed SELF with plaintext segments, same as Resogun's |
| Prepare the image | **works** — `Tempest 4000 \| entry=0xb5380 \| image=2,608,905 bytes` |
| Link the modules | **works** — libc, libSceFios2 and libSceS3DConversion (deferred); 120 native bindings |
| Boot | **no** — stops in libc init at `STOP: first unsupported PS4 import: rNhWz+lvOMU#libkernel` |

The game's entry point has not run yet, so nothing is drawn. Reproduce with:

```bash
python run.py check --any-build --game <extracted dump> --out out/tempest
```

Use `--any-build`: `VERIFIED_BUILDS` in `scripts/prepare.py` holds Resogun's eboot hashes, so any
other game's eboot is unverified by definition.

## What is missing

431 imported symbols, 3,782 relocations, 38 required modules. 439 imports fall to the host runtime
and **52 of them have no implementation**:

| Count | Library | Nature |
|---|---|---|
| 14 | `libkernel` | **blocking** — the first one stops libc init |
| 10 | `libSceNgs2` | audio; Resogun uses Wwise through `libSceAudioOut`, this game uses NGS2 |
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

## What is Resogun-specific in here

This runner was written for one game, so expect to find assumptions:

- `scripts/prepare.py`: `VERIFIED_BUILDS` holds Resogun eboot hashes; `DLC_LOADER_SLOTS` in
  `scripts/link.py` holds GOT addresses of Resogun's eboot 01.10 (harmless here, nothing imports
  `dlcldr`).
- `setup/setup.cpp`: `missing_game_files` expects Resogun's `stream.dat` and `audio.pck`. Tempest
  ships `assets/` and loose `shader_*.sb` files instead, so the setup will reject the folder.
- `src/runtime_pad.c`: the intro automation presses Cross on Resogun's title screen when
  AvPlayer opens `title.mp4`.
- `src/runtime_ajm.c`: assumes Wwise ADPCM and Vorbis.
- Names, env variables and log prefixes are all `RESOGUN_*` / `resogun`.

## Worth knowing before investing

Tempest 4000 already has an official PC release (Steam and GOG). Unlike Resogun, which is
PlayStation-exclusive, anyone wanting it on a PC can buy it there. That does not make this port
pointless, but it is a different proposition from the Resogun one.

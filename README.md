# Tempest 4000 (PS4) on PC

Runs Llamasoft's **Tempest 4000** (CUSA09856, v1.02) natively on Linux. The game's own x86-64 code
executes directly; the PS4 system libraries it calls are reimplemented in C, and its GNM rendering
goes through shadPS4's video core on Vulkan.

This began as [CrownParkComputing/resogun-recomp](https://github.com/CrownParkComputing/resogun-recomp),
itself a fork of [rcostanza/resogun-recomp](https://github.com/rcostanza/resogun-recomp). Their
authors wrote the loader, the HLE runtime and the renderer; everything here carries their shape,
renamed rather than rewritten. Same licence, GPL-2.0-or-later.

🚨 **No game files are included** 🚨 — you supply your own `.pkg` or a decrypted dump.

## State

| | |
|---|---|
| Boots, renders, plays | **yes** — title screen, level select, the tube, its own geometry |
| All 52 PS4 imports it needs | **implemented** |
| Audio | ATRAC9 through NGS2 with LibAtrac9; the decoder and mixer are written but **not yet confirmed to make sound** |
| Intro video | stalls on some runs (AvPlayer); moving `assets/Atari_h264_30fps.mp4` aside gets past it |
| Windows | no |
| Android | not yet; a proven route exists, see below |

[PORTING.md](PORTING.md) records what was measured and what each import needed.

## Rendering and display options

In the game, **F10** (or hold **Select**/View/Share for two seconds) opens the Display Settings
panel: window mode, aspect ratio, resolution, monitor, VSync, frame rate, render resolution,
sharpening and HDR. It applies at once and saves to `settings.ini`.

The same keys can be set before starting, in `settings.ini` beside the binary or in your save
folder (`settings.example.ini` documents every one), or per run as environment variables:

```bash
TEMPEST_DISPLAY_MODE=windowed TEMPEST_OUTPUT_RESOLUTION=1280x720 ./tempest
```

`display_mode=borderless` is the default and covers the screen; use `windowed` if you want the
desktop back while it runs.

## Android

Not yet, and not by rebuilding: the PS4's own x86-64 machine code is executed directly here, so
an ARM device cannot run it as it stands. But it is not out of reach, because the same problem
has already been solved for the emulator this renderer comes from.

[Bachata S4](https://github.com/JICA98/Bachata-S4) runs
[shadPS4 on ARM64 Android](https://github.com/zenithblue-oss/shadps4-arm64) and ships on Google
Play, GPL-2.0-or-later. Its shape is: the emulator itself **cross-compiled for arm64**, the PS4's
x86-64 guest code executed by **FEX**, Vulkan through **Turnip** and a **Vortek** passthrough
server, and glibc supplied by a managed Debian runtime. Its `runtime/scripts/` has a build script
for each piece.

The same route would work here, and the work is real but bounded: this runner's own C would have
to build for aarch64, and the places that execute or assume x86-64 would have to go through
FEXCore instead -- `loader.c` calling the game's entry as a function pointer, the x86-64 assembly
in `enter_on_stack`, the `cpuid` in `cpu_check.c`, the fs-to-gs patching and the ISA rewriter.
That is a port of the execution model, not a build flag.

## Features

- **30, 60, 90, 120, 144 or 180 FPS.** Higher framerates maybe possible in the future; 30 is for lower-powered PCs.
  - Note: if the game fails to reach the set FPS cap, it will run slower. So choose a cap that your device can reach.
- **4K support** (not native, the game's own checkerboard mode) or 1080p.
- **Aspect ratios (16:9, 16:10, 4:3, 21:9 and 32:9):** ultrawide shows more of the level to the sides,
  16:10 and 4:3 more above and below, nothing stretched. The menus stay 16:9.
- **DLC support** from your own DLC dumps.
- **Built in skip intro:** (optionally) boots straight to the main menu, the start-up logos and the "Press X" screen are skipped.
- **Display settings in the game:** press **F10**, or hold **Select** (View, Back, Share) for two
  seconds, for window mode, resolution, VSync, frame rate and more.
- **Keyboard and any gamepad.** DualShock 4 and DualSense work as they are, Xbox-style pads by
  button position.
- **Easy setup:** point the installer at your `.pkg`, which it extracts for you, and it can add a desktop shortcut (on Linux, an application menu entry).
- **Works with [RecompLauncher](https://recomplauncher.com):** it installs and updates the game from these releases, sets it up from your `.pkg` and shows the settings on the game's page.

## Requirements

- **Linux x86-64**, glibc 2.39 or newer (SteamOS 3.7, Ubuntu 24.04, Debian 13, Fedora 40 or
  newer, Arch), and Python 3.10 or newer (`python3`) for the setup. This fork is Linux only; the
  upstream project also builds for Windows.
- A CPU with AVX. The PS4's own x86-64 code runs directly, so an ARM machine cannot run it.
- A GPU with Vulkan 1.3 and a current driver (Mesa or the NVIDIA driver).
- The `.pkg` of your own copy of the game. Setup extracts it into a folder next to it, which
  takes a few seconds and about 700 MB. A dump you have already extracted works too: the folder
  with `eboot.bin`, `sce_module`, `sce_sys`, `audio.pck` and `stream.dat`.

A higher frame rate needs a PC that holds it: Tempest's game speed follows the frames it shows, so a frame that comes late slows the game down for that moment.

## Install and play

1. [Download the latest release here](https://github.com/CrownParkComputing/resogun-recomp/releases/latest)
2. Extract it anywhere you can write: `tar -xzf tempest-pc-<version>-linux.tar.gz -C ~/Games`
3. Run `tempest-setup`: from a file manager it asks with dialogs, in a terminal it asks there. It
   also takes `--game <pkg or dump> [--save <folder>] [--dlc <folder>]`.
4. Choose your game's **`.pkg`** (or a dump you have already extracted), optionally a **DLC
   folder**, and a **save folder**. It extracts the `.pkg` next to itself, prepares the game in a
   few seconds and offers an entry in your application menu. Your `.pkg` and your dumps are only
   read, never changed.
5. Start the game from the menu entry, or run `./tempest`.

Run `tempest-setup` again any time to change folders; it offers to copy your saves when the save
folder moves. Turn off **Skip the intro logos** (or set `skip_intro=0` in `settings.ini`) to see
the logos and the title screen again.

On a Steam Deck, do this in Desktop Mode; _Add a Non-Steam Game_ in Steam then lists the menu entry.

### Updating

Extract the new release over the old folder and run the setup once more (the game asks you to). Your folders, saves and settings are kept. Setup also remembers the folders in `~/.config/tempest/paths.ini`, so a release extracted somewhere new only needs **Install**.

### DLC

Tempest's add-ons (the extra ships and skins) carry no data of their own: the game only asks whether each one is installed. A DLC dump is a folder holding `sce_sys`, named after the add-on's label, the end of its content ID (`UP9000-CUSA00038_00-ASTRONAUT0000002` is `ASTRONAUT0000002`):

```
<folder containing your DLCs>/
  ASTRONAUT0000002/
    sce_sys/
  ...
```

## Settings

`settings.ini` in your save folder holds every setting, each with a comment. The **Display Settings** panel (**F10**, or hold **Select** for two seconds) changes most of them while the game runs and saves them when it closes:

- aspect ratio (4:3, 16:9, 16:10, 21:9 or 32:9, at the next start), window mode, windowed resolution (the sizes of
  that ratio; applied a second after you stop changing it) and monitor. The fullscreen modes pick their
  size from the screen and the aspect ratio, with black bars when the screen is of another shape;
- VSync, and tearing with VSync off (the least input delay);
- frame rate: 30, 60, 90, 120, 144 or 180 (30 for PCs that cannot hold 60), held to what your display refreshes at (180 on a 120 Hz display runs at 120) unless VSync is off with tearing allowed;
- render resolution (4K or 1080p, at the next start);
- sharpening (AMD FSR 1 RCAS, on by default), applied at once; **F9** also switches it over the game, to compare;
- HDR, the game's own HDR output on an HDR display (at the next start).

`title_back_exits=0` stops Back on the title screen from asking to close the game.

## Controls

The game sees one DualShock 4. Any gamepad works, mapped by button position: on an Xbox pad A B X Y
are Cross, Circle, Square, Triangle, LB/RB are L1/R1, LT/RT are L2/R2, Menu is Options and View
clicks the touchpad. The keyboard works alongside a gamepad:

| Keys                      | PS4                      |
| ------------------------- | ------------------------ |
| W A S D / I J K L         | left stick / right stick |
| arrow keys                | d-pad                    |
| Space, Enter              | Cross                    |
| Escape, Backspace, `;`    | Circle                   |
| F / H                     | Square / Triangle        |
| Q, Page Up / O, Page Down | L1 / R1                  |
| E / U                     | L2 / R2                  |
| X / M                     | L3 / R3                  |
| Tab, P                    | Options                  |
| T                         | touchpad click           |

| Shortcut                                | What it does                                |
| --------------------------------------- | ------------------------------------------- |
| **F10**, or hold **Select** 2 s         | Display Settings                            |
| **F9**                                  | sharpening on or off                        |
| **F4**, or **Back** on the title screen | asks "Exit Tempest?" (No is selected first) |
| **Alt+F4**                              | closes the game without confirmation        |

## When something goes wrong

The game logs to `tempest.log` in the `log` folder of your save folder. If it closes by itself, it
says so and keeps that log as `crash-<date>-<time>.log` in the same folder; attach that file when
you report the problem. If the picture freezes, leave the game running for half a minute before
closing it: after 20 seconds without a new frame it writes what every thread is doing to the log.

## Building from source

[docs/building.md](docs/building.md) covers the toolchain, building, tests and packaging, and
[docs/development.md](docs/development.md) how the runner works and how to work on it.

## 🤖 AI usage disclaimer 🤖

**_This project was thoroughly made by AI._**

I (human) am a programmer, but I know nothing about C++ which is required to make this possible. And so, I've done precisely three things in this project:

1. Paid for AI
2. Told AI what to do
3. Tested out whatever AI did

## Credits and license

Based on and inspired by [theatrhythm-fbl-pc](https://github.com/YAPhoa/theatrhythm-fbl-pc).

GPL-2.0-or-later, see [LICENSE](LICENSE). Runtime follows [shadPS4](https://github.com/shadps4-emu/shadPS4). Tempest is a game by Housemarque, published by Sony Interactive Entertainment; this project is not affiliated with or endorsed by either.

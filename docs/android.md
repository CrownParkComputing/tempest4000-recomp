# Android development

The development target is a Retroid Pocket Flip2: SM8250/Snapdragon 865, Adreno 650,
Android 13 (API 33), ARM64. Builds use Android API 31 or newer. This is experimental;
the title and gameplay run, with further rendering and sustained-audio checks in progress.

## Established on the device

- The native ARM64 NGS2 routing, timing, PS4 voice-event and looping checks pass.
- PCM effects support mono/stereo, sampler pitch and persistent resampling. Terminal voice
  events report EMPTY so Scream can recycle its effects pool. A sustained native run exceeded
  1,000 effect starts; an Android gameplay run exceeded 300 starts with finished voices reused.
- A 21-second decode of the real ATRAC9 title music matches the Linux WAV byte for byte.
- ATRAC9 loop/restart boundaries use a fresh decoder: configuring an existing LibAtrac9
  handle leaves its superframe position and overlap history intact, cutting off the title
  music at its first loop. The real title track passes 1,493 seconds of accelerated decoding
  on both Linux and Retroid ARM64; the regression also covers loops inside a superframe.
- FEX executes x86 calls into the actual C runtime, with typed integer, pointer, void,
  floating-point and stack arguments. Concurrent guest execution, GS TLS and unaligned
  loads pass.
- The real PS4 libc initializer registers its heap API, maps 210 MiB of flexible memory,
  and returns zero. Fios2 initializes and the original game entry reaches video output.
- The stock GPU driver reports Vulkan 1.1.128. The locally supplied
  `Turnip_v26.2.0_R5.zip` provides Vulkan 1.3.352, dynamic rendering, synchronization2,
  buffer device addresses, timeline semaphores, scalar block layout, int64 and geometry
  shaders. A separate native APK presents a 1920x1080 swapchain image successfully.
- The development game APK renders the correctly sized title and gameplay, plays the title
  music and PCM effects, and detects the Retroid controller. Tested steady scenes reach
  60 FPS; shader compilation and scene changes can interrupt that rate.
- Turnip lacks sparse buffers. The renderer uses fully backed buffer arenas instead.
  The ARM64 resource-table walker passes dynamic-offset, nested-pointer and code-reload checks.
- Single-sampled images are the default for Mesa Turnip: the game's mixed sample counts
  otherwise produce an enlarged, repeating picture on Adreno 650.
- Restoring saved Android pipelines can produce black frames. Android defaults to compiling
  shaders on demand each launch; the desktop shader cache remains available.

The GOG 1.07 installer supplied as a fallback remains at the user's original path;
the Android build below uses the PS4 dump and does not mix the two editions.

## Repeat the execution checks

The SDK defaults to `$HOME/Android/Sdk`. `ANDROID_NDK_HOME` overrides NDK selection;
otherwise the latest installed NDK is selected. An ADB serial is always explicit.

```bash
bash tools/check_android_audio.sh --device ADB_SERIAL
bash tools/build_android_fex.sh
bash tools/run_android_fex.sh ADB_SERIAL
bash tools/run_android_boot.sh ADB_SERIAL /path/to/boot-linked.bin
bash tools/run_android_boot.sh ADB_SERIAL /path/to/boot-linked.bin --entry
```

The last command is headless and deliberately stops at an unsupported graphics import.
The initializer-only command should finish with `real PS4 libc initializer returned 0`.
Its image must be RGIMAGE7, prepared from the same owned dump as the desktop runner.

The default FEX build is a native Android NDK executable. `--glibc` and
`tools/stage_android_runtime.py` retain the separately tested private glibc alternative;
it is not required by the Android APK.

## Driver check APK

Use a locally supplied ARM64 Mesa Turnip driver ZIP:

```bash
bash tools/build_android_gpu_check.sh /path/to/Turnip.zip
adb -s ADB_SERIAL install -r out/android-gpu-check/tempest-gpu-check.apk
adb -s ADB_SERIAL shell am start -n com.crownparkcomputing.tempest.gpucheck/android.app.NativeActivity
```

The result is in the app's external files directory, `gpu-check.log`. The purple test
image verifies Vulkan presentation, not game rendering. Its packaged
`driver-source.json` records the ZIP and library SHA-256 hashes and driver metadata.

## Experimental renderer build

FEXCore and the C runtime build without glibc. The renderer also compiles with the NDK,
using the existing SDL3 window, controller and audio implementations. The Android
surface and CPU helper changes leave desktop paths available.

The renderer currently expects a matching Android SDL3 install/source tree, a staged
header prefix containing Boost, Vulkan/Vulkan-Hpp, robin-map and VMA, and the Android
FFmpeg built by `tools/build_android_ffmpeg.sh`. Header staging must copy only these
architecture-independent headers; do not add the host `/usr/include` to an NDK build.

```bash
bash tools/build_android_ffmpeg.sh
cmake -S out/android-deps/fex -B out/android-fex-ndk \
  -DTEMPEST_ANDROID_RENDERER=ON \
  -DTEMPEST_ANDROID_HEADERS=/path/to/staged-headers \
  -DTEMPEST_ANDROID_SDL=/path/to/android-sdl3-install \
  -DTEMPEST_FFMPEG_DIR="$PWD/out/android-ffmpeg"
cmake --build out/android-fex-ndk --target main --parallel 8
TEMPEST_ANDROID_SDL_SOURCE=/path/to/matching-sdl3-source \
  bash tools/package_android.sh /path/to/Turnip.zip
bash tools/install_android.sh ADB_SERIAL /path/to/boot-linked.bin /path/to/ps4-dump
adb -s ADB_SERIAL shell am start -n com.crownparkcomputing.tempest4000/.TempestActivity
```

The final APK is `out/android-app/tempest4000.apk`, labelled **Tempest 4000**. It updates
the development app in place, retaining its saves. The title-focused launcher PNG in
`res/tempest-android-icon.png` fills the adaptive icon's visible area against a black
background. A second packaging argument or `TEMPEST_ANDROID_ICON` supplies alternative
artwork.

The runner writes `tempest.log` beside its external `boot-linked.bin`, `app0/` and
`user/`. The APK has no default watchdog; `TEMPEST_ANDROID_TIMEOUT=60` enables a bounded
development run. The APK contains the runtime, SDL, the selected driver and the supplied
launcher artwork; game data is copied separately into `app0/` by the install script.
Game data and extracted assets do not belong in the repository.

## Remaining integration

Point-sprite coordinates, flat point colors and large-point support restore pickups and
particles. Android also initializes the point-coordinate registers on drivers without
triangle-barycentric extensions; otherwise texture samples used undefined coordinates.
The Retroid now displays the pickup ring/+ graphics and background starfield, confirmed in
gameplay. Its emitted particle shader loads `PointCoord` and passes SPIR-V validation.

Cached pipeline restoration and Android pause/resume remain unresolved: returning from
another app can leave a blank surface. A fresh launch restores the development build.
Full guest thread-exit cleanup and AvPlayer guest callbacks still need to be checked through
FEX. The default skips the intro.

For device diagnostics, `launch.ini` in the external files directory accepts literal
`TEMPEST_NAME=value` lines. `TEMPEST_FRAME_STATS=1` logs measured frame rate. Creating the
file named by `TEMPEST_SCREENSHOT_TRIGGER` saves the game's frame under `user/screenshots/`;
`TEMPEST_CAPTURE_TRIGGER` records a draw/resource report under `user/captures/`.

Pinned sources: FEXCore `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`, embedding bridge
`zenithblue-oss/shadps4-arm64` `be6bc2e9c60799e071dd2fafa6216e8d80ec619c`,
libadrenotools `8fae8ce254dfc1344527e05301e43f37dea2df80`, and its liblinkernsbypass
submodule `aa3975893d83ef1bc84c321ec60c65fbf1287887`. FFmpeg uses the same 9.0.2
archive hash as the desktop build. Driver binaries are supplied locally and are not
checked into this repository.

Driver selection is now available from the launcher icon; see [graphics driver settings](android-driver-selection.md). The bundled Turnip remains the default.

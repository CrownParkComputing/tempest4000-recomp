# Build Tempest 4000 for Android

The tested device is a Retroid Pocket Flip2 (ARM64, Android 13, Adreno 650).
The app is `com.crownparkcomputing.tempest4000/.TempestActivity` and keeps its
own files and saves separately from PAC-MAN CE2.

Install the Linux development dependencies described in `docs/building.md`,
JDK 17+, `zip`, Android SDK platform/build-tools/platform-tools and an NDK.
The tested NDK is 30.0.14904198. From this checkout:

```bash
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/30.0.14904198"
export PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"
bash tools/setup_android_deps.sh
source out/android-env.sh
bash tools/build_android_fex.sh --ndk
```

Dependency setup stages only architecture-independent host renderer headers,
builds hash-checked SDL3 3.4.16 and FFmpeg 9.0.2, and fetches pinned AdrenoTools.
The FEX builder fetches its pinned checkout and applies the included embedding
patch. All generated dependencies remain under `out/`; another port's build
directory is not required.

Supply an ARM64 AdrenoTools Turnip ZIP when packaging:

```bash
bash tools/package_android.sh /path/to/Turnip.zip
```

The tested shared PS4 build is Standard Adreno 6xx/7xx Turnip
`Turnip-v27.0.0-20261111-r2.zip`, with ZIP SHA-256
`bcc118d8a8bb4206f2012c1941252f46eb92b3da5cc579af368ac56fb2769271`.
The package records exact driver provenance in `driver-source.json`.
The output is `out/android-app/tempest4000.apk`, signed with the existing local
`$HOME/.android/debug.keystore`; keep that key for subsequent updates.

Prepare your owned decrypted PS4 game on the host first. See `PORTING.md` for
the tested CUSA09856 build. The shared preparation gate still names the inherited
base title, so this port uses `--any-build` for that identified Tempest 4000 dump:

```bash
python3 run.py prepare --game /path/to/Tempest4000 --any-build
bash tools/install_android.sh ADB_SERIAL out/game/boot-linked.bin /path/to/Tempest4000
adb -s ADB_SERIAL shell am start -n com.crownparkcomputing.tempest4000/.TempestActivity
```

For an APK-only update, use `adb -s ADB_SERIAL install -r
out/android-app/tempest4000.apk`. No uninstall or save replacement is needed.
Game data remains outside the APK under the app's external files directory.

Bundled Turnip is the default. Long-press the launcher icon → **Graphics driver**
to select the system driver or import another ARM64 driver ZIP. Settings apply
on the next game launch, with independent per-game preferences. See
[`docs/android-driver-selection.md`](../docs/android-driver-selection.md).

Focused importer checks: `bash tools/check_android_driver_import.sh`.
The inherited runtime/device investigation is in `docs/android.md`.

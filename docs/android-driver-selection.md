# Graphics drivers in the PS4 Android ports

Tempest 4000 and PAC-MAN CE2 default to the Turnip driver bundled in their APK.
The driver setting is separate for each game.

## Change a driver

1. Long-press the game's launcher icon and choose **Graphics driver**. Android's
   app-info screen may also expose its app preferences.
2. Select **Bundled** or **System Vulkan driver**, or choose **Import driver ZIP**
   to select an ARM64 AdrenoTools driver package from the document picker.
3. Close and reopen the game to apply the selection. Choosing a driver does not
   restart a running level or change its saves.

An imported package appears as a third choice, using the name from its metadata.
To restore the default, select **Bundled**. The settings activity works independently
of the game renderer, including when a chosen driver cannot start the game.
A system driver still needs to provide the Vulkan features the renderer requires.

Imported ZIPs need `meta.json` naming the driver library with `libraryName`.
Only a little-endian ARM64 shared library is accepted, and an incompatible
`minApi` is refused. The importer reads only the metadata and that library, with
bounded archive/library sizes. It stores each distinct library at an immutable,
hash-named private path so another import cannot overwrite a driver the game
has already loaded. Failed imports retain the previous selection.

The UI retains the existing single launcher icon per game. Driver settings are
an icon shortcut, not another installed game or a replacement package.

## Build and verify

The three Java sources under `android/java/com/crownparkcomputing/ps4runtime/`
and `android/graphics_driver.h` are the common implementation used in both ports.
The packaging scripts compile every Java source under `android/java/`.
The activity exposes `getGraphicsDriverPath()` to the native loader and registers
the launcher shortcut. The manifest also declares `GraphicsDriverActivity` for
renderer-independent access and Android app preferences.

Run the focused import checks with a host JDK:

```bash
bash tools/check_android_driver_import.sh
```

An optional real driver ZIP and expected library SHA-256 can be supplied:

```bash
bash tools/check_android_driver_import.sh /path/to/Turnip.zip EXPECTED_LIBRARY_SHA256
```

Fixtures are generated, without game files or driver execution. Checks cover
successful imports, unchanged bytes, repeated imports, immutable paths,
malformed/missing metadata, traversal, wrong ELF/ABI, Android API rejection,
and cleanup after failure. Android packaging compiles against the SDK; actual
driver compatibility still depends on the target GPU and driver build.

The native startup log records whether the bundled, system or imported driver
was selected. Imported paths include the library SHA-256. Preferences live in
app-private `graphics_driver` shared preferences, separate from game saves.

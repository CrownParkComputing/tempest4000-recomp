#!/usr/bin/env bash
# Package the Android runner and supplied launcher artwork; game data stays external.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
port=$PWD
driver=${1:?Usage: package_android.sh TURNIP_DRIVER_ZIP [ICON_PNG]}
driver=$(realpath "$driver")
icon=${2:-${TEMPEST_ANDROID_ICON:-$port/res/tempest-android-icon.png}}
[[ -f $icon ]] || { echo 'Pass the Tempest launcher PNG as the second argument or TEMPEST_ANDROID_ICON.' >&2; exit 1; }
icon=$(realpath "$icon")
build=${TEMPEST_FEX_BUILD:-$port/out/android-fex-ndk}
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $ndk ]]; then
    mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
    for ((i=${#installed[@]}-1; i>=0; --i)); do
        if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
    done
fi
mapfile -t build_tools < <(printf '%s\n' "$sdk"/build-tools/*/aapt2 | sort -V)
bt=${build_tools[-1]%/aapt2}
mapfile -t platforms < <(printf '%s\n' "$sdk"/platforms/android-*/android.jar | sort -V)
platform=${platforms[-1]}
sdl=${TEMPEST_ANDROID_SDL:-$(sed -n 's/^TEMPEST_ANDROID_SDL:PATH=//p' "$build/CMakeCache.txt")}
sdl_source=${TEMPEST_ANDROID_SDL_SOURCE:-$(dirname "$sdl")/sdl-src}
[[ -f $build/android-libs/libmain.so && -f $sdl/lib/libSDL3.so ]] || {
    echo 'Build the Android main target and set TEMPEST_ANDROID_SDL first.' >&2; exit 1;
}
[[ -d $sdl_source/android-project/app/src/main/java/org/libsdl/app ]] || {
    echo 'Set TEMPEST_ANDROID_SDL_SOURCE to the matching SDL3 source.' >&2; exit 1;
}
package=$port/out/android-app/package
mkdir -p "$package/classes" "$package/lib/arm64-v8a" "$package/assets"
resources=$port/out/android-app/resources
mkdir -p "$resources/drawable-nodpi" "$resources/mipmap-anydpi-v26"
cp "$icon" "$resources/drawable-nodpi/tempest_game_icon.png"
cp android/res/mipmap-anydpi-v26/ic_launcher.xml "$resources/mipmap-anydpi-v26/ic_launcher.xml"
"$bt/aapt2" compile --dir "$resources" -o "$package/resources.zip"
python3 tools/stage_android_driver.py "$driver" "$package/assets"
mapfile -t app_sources < <(rg --files android/java -g '*.java')
javac -nowarn -source 17 -target 17 -classpath "$platform" -d "$package/classes" \
    "$sdl_source"/android-project/app/src/main/java/org/libsdl/app/*.java \
    "${app_sources[@]}"
mapfile -t classes < <(rg --files "$package/classes" -g '*.class')
"$bt/d8" --release --min-api 31 --lib "$platform" --output "$package" "${classes[@]}"
cp "$build"/android-libs/*.so "$package/lib/arm64-v8a/"
cp "$sdl/lib/libSDL3.so" "$package/lib/arm64-v8a/"
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$package/lib/arm64-v8a/"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$package"/lib/arm64-v8a/*.so
"$bt/aapt2" link -I "$platform" --manifest "$port/android/AndroidManifest.xml" \
    --min-sdk-version 31 --target-sdk-version 33 -R "$package/resources.zip" \
    -A "$package/assets" -o "$package/base.apk"
(cd "$package" && zip -q -r base.apk classes.dex lib)
"$bt/zipalign" -f -p 4 "$package/base.apk" "$package/aligned.apk"
keystore=$HOME/.android/debug.keystore
[[ -f $keystore ]] || { echo 'Create an Android debug keystore first.' >&2; exit 1; }
"$bt/apksigner" sign --ks "$keystore" --ks-pass pass:android --ks-key-alias androiddebugkey \
    --key-pass pass:android --out "$port/out/android-app/tempest4000.apk" "$package/aligned.apk"
"$bt/apksigner" verify "$port/out/android-app/tempest4000.apk"
echo "Built $port/out/android-app/tempest4000.apk"

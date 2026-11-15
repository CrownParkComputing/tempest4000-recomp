#!/usr/bin/env bash
# Package the native Vulkan/Turnip probe. The input ZIP stays outside the repository.
# Usage: build_android_gpu_check.sh /path/to/Turnip.zip
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
port=$PWD
driver=${1:?Usage: build_android_gpu_check.sh TURNIP_DRIVER_ZIP}
driver=$(realpath "$driver")
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $ndk ]]; then
    mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
    for ((i=${#installed[@]}-1; i>=0; --i)); do
        if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
    done
fi
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo 'Set ANDROID_NDK_HOME.' >&2; exit 1; }
mapfile -t build_tools < <(printf '%s\n' "$sdk"/build-tools/*/aapt2 | sort -V)
bt=${build_tools[-1]%/aapt2}
mapfile -t platforms < <(printf '%s\n' "$sdk"/platforms/android-*/android.jar | sort -V)
platform=${platforms[-1]}
[[ -f $platform && -x $bt/aapt2 ]] || { echo 'Need an Android SDK platform and build-tools.' >&2; exit 1; }
revision=8fae8ce254dfc1344527e05301e43f37dea2df80
source_dir=${TEMPEST_ADRENOTOOLS_SOURCE:-$port/out/android-deps/adrenotools}
if [[ ! -e $source_dir/.git ]]; then
    git clone https://github.com/bylaws/libadrenotools.git "$source_dir"
    git -C "$source_dir" checkout --detach "$revision"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "libadrenotools must be pinned to $revision" >&2; exit 1;
}
git -C "$source_dir" submodule update --init --depth 1
build=$port/out/android-gpu-check
cmake -S android/gpu-check -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DTEMPEST_ADRENOTOOLS_SOURCE="$source_dir" -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$build/lib/arm64-v8a"
cmake --build "$build" --target tempest-gpu-check --parallel "${TEMPEST_BUILD_JOBS:-8}"
package=$build/package
mkdir -p "$package/lib/arm64-v8a" "$package/assets"
cp "$build"/lib/arm64-v8a/*.so "$package/lib/arm64-v8a/"
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$package/lib/arm64-v8a/"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$package"/lib/arm64-v8a/*.so
python3 tools/stage_android_driver.py "$driver" "$package/assets"
"$bt/aapt2" link -I "$platform" --manifest "$port/android/gpu-check/AndroidManifest.xml" \
    --min-sdk-version 31 --target-sdk-version 33 -A "$package/assets" -o "$package/base.apk"
(cd "$package" && zip -q -r base.apk lib)
"$bt/zipalign" -f -p 4 "$package/base.apk" "$package/aligned.apk"
keystore=$HOME/.android/debug.keystore
if [[ ! -f $keystore ]]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Android Debug'
fi
"$bt/apksigner" sign --ks "$keystore" --ks-pass pass:android --ks-key-alias androiddebugkey \
    --key-pass pass:android --out "$build/tempest-gpu-check.apk" "$package/aligned.apk"
"$bt/apksigner" verify "$build/tempest-gpu-check.apk"
echo "Built $build/tempest-gpu-check.apk"

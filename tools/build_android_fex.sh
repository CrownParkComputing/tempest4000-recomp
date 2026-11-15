#!/usr/bin/env bash
# Build Tempest's FEXCore execution check as native Android ARM64 code.
# --glibc selects the separately tested Linux/glibc embedding build instead.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
port=$PWD
mode=${1:---ndk}
[[ $mode == --ndk || $mode == --glibc ]] || { echo 'Usage: build_android_fex.sh [--ndk|--glibc]' >&2; exit 2; }
revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec
source_dir=${TEMPEST_FEX_SOURCE:-$port/out/android-deps/fex}
build_dir=${TEMPEST_FEX_BUILD:-$port/out/android-fex-ndk}
for tool in git cmake ninja clang clang++; do
    command -v "$tool" >/dev/null || { echo "Need $tool for the ARM64/glibc build." >&2; exit 1; }
done
toolchain=()
if [[ $mode == --ndk ]]; then
    sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [[ -z $ndk ]]; then
        mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
        for ((i=${#installed[@]}-1; i>=0; --i)); do
            if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
        done
    fi
    [[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo 'Set ANDROID_NDK_HOME to an installed NDK.' >&2; exit 1; }
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31)
else
    command -v aarch64-linux-gnu-gcc >/dev/null || { echo 'Need aarch64-linux-gnu-gcc for --glibc.' >&2; exit 1; }
    build_dir=${TEMPEST_FEX_BUILD:-$port/out/android-fex}
    toolchain=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu
        -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu)
fi
if [[ ! -d $source_dir/.git ]]; then
    mkdir -p "$(dirname -- "$source_dir")"
    git clone --filter=blob:none --no-checkout --depth 1 https://github.com/FEX-Emu/FEX.git "$source_dir"
    git -C "$source_dir" fetch --depth 1 origin "$revision"
    git -C "$source_dir" checkout --detach "$revision"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "FEX must be at $revision; refusing to replace an existing checkout." >&2; exit 1;
}
patch=$port/android/fexcore-only.patch
if git -C "$source_dir" apply --check "$patch" 2>/dev/null; then
    git -C "$source_dir" apply "$patch"
elif ! git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
    echo 'The FEX embedding patch does not match this checkout.' >&2; exit 1
fi
git -C "$source_dir" submodule update --init --depth 1 --jobs 8 -- \
    External/unordered_dense External/rpmalloc External/xxhash External/fmt \
    External/range-v3 Source/Common/cpp-optparse
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=TRUE -DTUNE_CPU=none \
    -DBUILD_FEXCORE_ONLY=ON -DFEXCORE_EMBED_PROJECT="$port/android/FexTarget.cmake" \
    -DBUILD_TESTING=OFF -DBUILD_FEX_LINUX_TESTS=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF \
    -DENABLE_GDB_SYMBOLS=OFF -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_VIXL_DISASSEMBLER=OFF -DENABLE_VIXL_SIMULATOR=OFF \
    -DENABLE_ZYDIS=OFF -DENABLE_FEXCORE_PROFILER=OFF
cmake --build "$build_dir" --target tempest-fex-check tempest-boot-check --parallel "${TEMPEST_BUILD_JOBS:-8}"
echo "Built $build_dir/Bin/tempest-fex-check"
echo "Built $build_dir/Bin/tempest-boot-check"

#!/usr/bin/env bash
# Prepare this checkout's Android dependencies without another port's build tree.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
port=$PWD
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $ndk ]]; then
    mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
    for ((i=${#installed[@]}-1; i>=0; --i)); do
        if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
    done
fi
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo 'Set ANDROID_NDK_HOME to an installed NDK.' >&2; exit 1; }
for tool in git curl cmake ninja python3 pkg-config make tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Need $tool (docs/building.md)." >&2; exit 1; }
done
deps=$port/out/android-deps
mkdir -p "$deps" out/downloads out/android-headers/include
# Only architecture-independent headers: never copy the host's entire /usr/include.
python3 - <<'PY'
import os
from pathlib import Path
import shutil
prefixes = [Path(p) for p in os.environ.get('TEMPEST_ANDROID_HEADER_PREFIXES', 'out/vma:/usr/local:/usr').split(':')]
dest = Path('out/android-headers/include')
for name in ('boost', 'tsl', 'vulkan', 'vk_video', 'vk_mem_alloc.h'):
    source = next((p / 'include' / name for p in prefixes if (p / 'include' / name).exists()), None)
    if source is None:
        raise SystemExit(f'Missing host header {name}; install the Linux dependencies first (docs/building.md).')
    if source.is_dir():
        shutil.copytree(source, dest / name, dirs_exist_ok=True)
    else:
        shutil.copy2(source, dest / name)
PY
sdl_version=3.4.16
sdl_sha=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
archive=$port/out/downloads/SDL3-$sdl_version.tar.gz
if [[ ! -f $archive ]]; then
    curl -fL --retry 3 -o "$archive.part" "https://github.com/libsdl-org/SDL/releases/download/release-$sdl_version/SDL3-$sdl_version.tar.gz"
    mv "$archive.part" "$archive"
fi
echo "$sdl_sha  $archive" | sha256sum -c --status || { echo 'SDL archive hash mismatch.' >&2; exit 1; }
if [[ ! -d $deps/sdl-src ]]; then
    mkdir -p "$deps/sdl-src"
    tar -xzf "$archive" -C "$deps/sdl-src" --strip-components=1
fi
cmake -S "$deps/sdl-src" -B "$deps/sdl-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps/SDL3" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
cmake --build "$deps/sdl-build" --parallel "${TEMPEST_BUILD_JOBS:-8}"
cmake --install "$deps/sdl-build"
revision=8fae8ce254dfc1344527e05301e43f37dea2df80
adreno=$deps/adrenotools
if [[ ! -e $adreno/.git ]]; then
    git clone --filter=blob:none --no-checkout https://github.com/bylaws/libadrenotools.git "$adreno"
    git -C "$adreno" checkout --detach "$revision"
fi
[[ $(git -C "$adreno" rev-parse HEAD) == "$revision" ]] || { echo "libadrenotools must be at $revision." >&2; exit 1; }
git -C "$adreno" submodule update --init --recursive --depth 1
bash tools/build_android_ffmpeg.sh
# Bash-escaped paths keep the recipe usable when the checkout directory has spaces.
{
    printf 'export ANDROID_SDK_ROOT=%q\n' "$sdk"
    printf 'export ANDROID_NDK_HOME=%q\n' "$ndk"
    printf 'export TEMPEST_ANDROID_RENDERER=1\n'
    printf 'export TEMPEST_ANDROID_SDL=%q\n' "$deps/SDL3"
    printf 'export TEMPEST_ANDROID_SDL_SOURCE=%q\n' "$deps/sdl-src"
    printf 'export TEMPEST_ANDROID_HEADERS=%q\n' "$port/out/android-headers"
    printf 'export TEMPEST_FFMPEG_DIR=%q\n' "$port/out/android-ffmpeg"
    printf 'export TEMPEST_ADRENOTOOLS_SOURCE=%q\n' "$adreno"
} > out/android-env.sh
echo 'Dependencies ready. Run: source out/android-env.sh && bash tools/build_android_fex.sh --ndk'

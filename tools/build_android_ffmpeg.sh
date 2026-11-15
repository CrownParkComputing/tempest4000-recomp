#!/usr/bin/env bash
# The Android build of the same minimal FFmpeg used by the desktop runner.
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
bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
[[ -x $bin/aarch64-linux-android31-clang ]] || { echo 'Set ANDROID_NDK_HOME.' >&2; exit 1; }
version=9.0.2
sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
prefix=$port/out/android-ffmpeg
options=(--disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --disable-debug --enable-static --disable-shared --enable-zlib
    --enable-decoder=h264,aac,png --enable-parser=h264,aac,png --enable-demuxer=mov --enable-protocol=file
    --enable-swscale --enable-swresample --enable-cross-compile --target-os=android --arch=aarch64)
stamp="$version $ndk ${options[*]}"
if [[ -f $prefix/.stamp && $(<"$prefix/.stamp") == "$stamp" ]]; then exit 0; fi
mkdir -p out/downloads
archive=$port/out/downloads/ffmpeg-$version.tar.xz
if [[ ! -f $archive ]]; then
    curl -fsSL --retry 3 -o "$archive.part" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
    mv "$archive.part" "$archive"
fi
echo "$sha256  $archive" | sha256sum -c --status || { echo 'FFmpeg archive hash mismatch.' >&2; exit 1; }
work=$port/out/android-ffmpeg-build
mkdir -p "$work" "$prefix"
tar -xJf "$archive" -C "$work" --strip-components=1
(
    cd "$work"
    ./configure --prefix="$prefix" "${options[@]}" \
        --cc="$bin/aarch64-linux-android31-clang" --cxx="$bin/aarch64-linux-android31-clang++" \
        --ar="$bin/llvm-ar" --nm="$bin/llvm-nm" --ranlib="$bin/llvm-ranlib" --strip="$bin/llvm-strip"
    make -j"${TEMPEST_BUILD_JOBS:-8}"
    make install
) > "$work/build.log" 2>&1 || { tail -30 "$work/build.log" >&2; exit 1; }
echo "$stamp" > "$prefix/.stamp"
echo "Built Android FFmpeg in $prefix"

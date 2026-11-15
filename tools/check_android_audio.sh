#!/usr/bin/env bash
# Cross-build the existing NGS2 regression test and the real ATRAC9 extraction tool for Android.
# This is an audio smoke check, not an Android game build. No game files are required.
#   tools/check_android_audio.sh [--device ADB_SERIAL]
# ANDROID_NDK_HOME selects the NDK; otherwise use the newest one in the Android SDK.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

device=
while (($#)); do
    case $1 in
    --device) device=${2:?--device needs an ADB serial}; shift ;;
    -h|--help) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option $1 (--help lists the options)." >&2; exit 2 ;;
    esac
    shift
done

sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $ndk && -d $sdk/ndk ]]; then
    mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
    for ((i=${#installed[@]}-1; i>=0; --i)); do
        if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
    done
fi
cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
if [[ -z $ndk || ! -x $cc ]]; then
    echo 'Set ANDROID_NDK_HOME to an installed Linux x86-64 Android NDK.' >&2
    exit 1
fi

out=$PWD/out/android-audio
mkdir -p "$out"
"$cc" -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Werror \
    -I. -Isrc -Ithird_party/libatrac9 tests/test_ngs2.c -lm -o "$out/ngs2-test"
# LibAtrac9 is upstream C99 code, built with the same relaxed warnings as the Linux game.
"$cc" -std=c99 -O2 -w -Ithird_party/libatrac9 \
    tools/extract_at9.c third_party/libatrac9/*.c -lm -o "$out/extract_at9"
echo "Built Android ARM64 audio tools (API 31) in $out"

if [[ -n $device ]]; then
    adb=${ADB:-$sdk/platform-tools/adb}
    if [[ ! -x $adb ]]; then adb=$(command -v adb); fi
    "$adb" -s "$device" push "$out/ngs2-test" /data/local/tmp/tempest-ngs2-test
    "$adb" -s "$device" shell /data/local/tmp/tempest-ngs2-test
fi

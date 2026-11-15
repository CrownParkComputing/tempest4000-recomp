#!/usr/bin/env bash
# Install the APK and stage the user's own game separately.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
device=${1:?Usage: install_android.sh ADB_SERIAL BOOT_LINKED_BIN GAME_DIR}
image=${2:?Usage: install_android.sh ADB_SERIAL BOOT_LINKED_BIN GAME_DIR}
game=${3:?Usage: install_android.sh ADB_SERIAL BOOT_LINKED_BIN GAME_DIR}
[[ -f $image && -f $game/eboot.bin ]] || { echo 'Need a prepared image and decrypted PS4 game directory.' >&2; exit 1; }
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
adb=${ADB:-$sdk/platform-tools/adb}
[[ -x $adb ]] || adb=$(command -v adb)
package=com.crownparkcomputing.tempest4000
remote=/sdcard/Android/data/$package/files
"$adb" -s "$device" install -r out/android-app/tempest4000.apk
"$adb" -s "$device" shell mkdir -p "$remote/app0" "$remote/user"
"$adb" -s "$device" push "$image" "$remote/boot-linked.bin"
"$adb" -s "$device" push "$game/." "$remote/app0/"
echo "Installed Tempest 4000 and staged game for $package"

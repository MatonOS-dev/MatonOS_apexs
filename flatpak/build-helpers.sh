#!/bin/sh
# Build the device helper programs with the same pinned NDK as the Flatpak stack.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
OUT=${OUT_DIR:-$HERE/out}
mkdir -p "$OUT"
COMMON='-target x86_64-linux-android36 -Os -ffunction-sections -fdata-sections -Wl,--gc-sections'
# matonos-app-exec runs inside the app sandbox (no /system linker there), so it
# must be fully static. The other helpers run on the Android side and link the
# system's bionic dynamically.
"$CLANG" $COMMON -I"$HERE/helpers" -static "$HERE/helpers/matonos-app-exec.c" -o "$OUT/matonos-app-exec"
"$CLANG" $COMMON -I"$HERE/helpers" "$HERE/helpers/matonos-bwrap.c" -o "$OUT/matonos-bwrap"
"$CLANG" $COMMON -I"$HERE/helpers" "$HERE/helpers/linux/flatpak/flatpak-env-wrapper.c" -o "$OUT/flatpak-env-wrapper"
"$CLANG" $COMMON -I"$HERE/helpers" "$HERE/helpers/install/linuxd/FlatpakStore.c" -o "$OUT/matonos-flatpak-store"
for bin in matonos-app-exec matonos-bwrap flatpak-env-wrapper matonos-flatpak-store; do
  "$STRIP" --strip-all "$OUT/$bin"
done

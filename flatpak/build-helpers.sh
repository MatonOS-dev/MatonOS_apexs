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
# Linux layer v3: only the install-staging helper is built here. The app
# launch helpers (flatpak-env-wrapper, matonos-app-tool -> matonos-app-flatpak /
# matonos-app-bwrap) are built in the MatonOS device tree; the v2 privileged
# matonos-bwrap and matonos-app-exec are retired (the compat proxy runs bwrap
# inside the app sandbox).
"$CLANG" $COMMON -I"$HERE/helpers" "$HERE/helpers/install/linuxd/FlatpakStore.c" -o "$OUT/matonos-flatpak-store"
for bin in matonos-flatpak-store; do
  "$STRIP" --strip-all "$OUT/$bin"
done

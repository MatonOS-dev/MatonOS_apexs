#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=${ARCH:-$(uname -m)}
SCRATCH=${SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
ROOTFS=$SCRATCH/rootfs/$ARCH
INPUTS=${ALPINE_INPUTS_DIR:-$SCRATCH/inputs}
GPGME_LITE_SRC=${GPGME_LITE_SRC:-$(CDPATH= cd -- "$HERE/../../DullPGP" && pwd)}
[ -x "$ROOTFS/bin/sh" ] || { echo "Missing offline Alpine build root: $ROOTFS" >&2; exit 1; }
[ "${HELPERS_ONLY:-0}" = 1 ] || [ -f "$GPGME_LITE_SRC/Makefile" ] || { echo "Missing DullPGP source tree: $GPGME_LITE_SRC" >&2; exit 1; }
[ "${HELPERS_ONLY:-0}" = 1 ] || { [ -d "$INPUTS/alpine-bin" ] && [ -d "$INPUTS/alpine-src" ]; } || {
  echo "Missing Alpine build inputs under $INPUTS (restore pinned release assets)" >&2; exit 1;
}
APK_INPUTS=$INPUTS/alpine-bin
SRC_INPUTS=$INPUTS/alpine-src
[ -d "$APK_INPUTS/alpine-bin" ] && APK_INPUTS=$APK_INPUTS/alpine-bin
[ -d "$SRC_INPUTS/alpine-src" ] && SRC_INPUTS=$SRC_INPUTS/alpine-src
if [ "${1##*/}" = build-static.sh ]; then
  PRUNED_SRC=$SCRATCH/alpine-src-pruned
  PRUNE_STAMP=$(sha256sum "$HERE/alpine-assets.lock" | awk '{print $1}')
  if [ "$(cat "$PRUNED_SRC/.matonos-prune-stamp" 2>/dev/null || true)" != "$PRUNE_STAMP" ]; then
    rm -rf "$PRUNED_SRC"
    mkdir -p "$PRUNED_SRC"
    cp -a "$SRC_INPUTS/." "$PRUNED_SRC/"
    rm -rf "$PRUNED_SRC/libidn2" "$PRUNED_SRC/libpsl" "$PRUNED_SRC/libunistring"
    printf '%s\n' "$PRUNE_STAMP" > "$PRUNED_SRC/.matonos-prune-stamp"
  fi
  SRC_INPUTS=$PRUNED_SRC
fi
if /usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --bind "$SCRATCH" /scratch \
  --bind "$HERE" /work \
  --ro-bind "$APK_INPUTS" /work/alpine-bin \
  --ro-bind "$SRC_INPUTS" /work/alpine-src \
  --bind "$GPGME_LITE_SRC" /work-gpgme-lite \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-net --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root \
  --setenv ARCH "$ARCH" \
  --setenv FLATPAK_REPO /scratch/flatpak-work \
  --setenv FLATPAK_COMMIT 03e6b205d01c9560a829941d95065a7c683d9667 \
  "$@"; then
  status=0
else
  status=$?
fi
[ "$status" -eq 0 ] || exit "$status"
case "${1##*/}" in
  build-static.sh)
    python3 "$HERE/license-gate.py" \
      --build "$SCRATCH/build" --output "$SCRATCH/output" \
      --alpine-src "$SRC_INPUTS" --apk-lock "$HERE/apk.lock" \
      --dullpgp "$GPGME_LITE_SRC" --repo-flatpak "$HERE" ;;
esac

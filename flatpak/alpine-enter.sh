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
exec /usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --bind "$SCRATCH" /scratch \
  --bind "$HERE" /work \
  --ro-bind "$INPUTS/alpine-bin" /work/alpine-bin \
  --ro-bind "$INPUTS/alpine-src" /work/alpine-src \
  --bind "$GPGME_LITE_SRC" /work-gpgme-lite \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-net --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root \
  --setenv ARCH "$ARCH" \
  --setenv FLATPAK_REPO /scratch/flatpak-work \
  --setenv FLATPAK_COMMIT 03e6b205d01c9560a829941d95065a7c683d9667 \
  "$@"

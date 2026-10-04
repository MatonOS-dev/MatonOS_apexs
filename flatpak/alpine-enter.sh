#!/bin/sh
set -eu
SCRATCH=/mnt/data/aosp/out/pc-logs/musl-static-116
ROOTFS=$SCRATCH/rootfs
exec /usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --proc /proc --dev /dev --tmpfs /tmp \
  --bind "$SCRATCH" /scratch \
  --bind /mnt/data/aosp/out/worktrees/musl-static-116/linux/flatpak-static /work \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root \
  --unshare-user --unshare-ipc --unshare-pid --uid 0 "$@"

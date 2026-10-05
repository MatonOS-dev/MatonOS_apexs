#!/bin/sh
set -eu
WORK=/work
OUT=/scratch/output
JOBS=${JOBS:-16}
[ "$JOBS" -le 16 ] || { echo 'JOBS must be <= 16' >&2; exit 2; }
mkdir -p "$OUT"
(cd "$WORK" && sha256sum -c helpers/SOURCE.sha256)
CFLAGS='-Os -ffunction-sections -fdata-sections'
for helper in matonos-bwrap matonos-app-exec; do
  cc -std=c11 -Wall -Wextra -Werror $CFLAGS -I/work/helpers \
    -static-pie -Wl,--gc-sections \
    "/work/helpers/$helper.c" -o "$OUT/$helper.unstripped"
  mv "$OUT/$helper.unstripped" "$OUT/$helper"
  strip --strip-all "$OUT/$helper"
done
# Host launch probe variant: same musl toolchain and release flags, with only
# test-only machine-id/udev path redirection compiled in. It is not staged.
cc -std=c11 -Wall -Wextra -Werror $CFLAGS -I/work/helpers \
  -DMATONOS_HOST_LAUNCH_PROBE -static-pie -Wl,--gc-sections \
  /work/helpers/matonos-bwrap.c -o "$OUT/matonos-bwrap-host-probe.unstripped"
mv "$OUT/matonos-bwrap-host-probe.unstripped" "$OUT/matonos-bwrap-host-probe"
strip --strip-all "$OUT/matonos-bwrap-host-probe"
checkdir="$OUT/apex-check"
rm -rf "$checkdir"
mkdir -p "$checkdir"
for binary in matonos-flatpak matonos-bwrap matonos-app-exec; do
  ln -s "../$binary" "$checkdir/$binary"
done
"$WORK/check-static-apex.sh" "$checkdir"
for helper in matonos-bwrap matonos-app-exec; do
  printf '%s %s bytes\n' "$helper" "$(stat -c %s "$OUT/$helper")"
done

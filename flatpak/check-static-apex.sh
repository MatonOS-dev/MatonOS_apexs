#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 PREBUILT_ARCH_DIR" >&2
  exit 2
fi
dir=$1
found=0
for binary in "$dir"/*; do
  [ -f "$binary" ] && [ -x "$binary" ] || continue
  found=1
  name=${binary##*/}
  if readelf -SW "$binary" | grep -E '\.(debug[^[:space:]]*|zdebug[^[:space:]]*|gnu_debuglink)([[:space:]]|$)' >/dev/null; then
    echo "ERROR: $name contains debug sections" >&2
    exit 1
  fi
  if readelf -l "$binary" | grep -q INTERP; then
    echo "ERROR: $name has PT_INTERP" >&2
    exit 1
  fi
  if readelf -d "$binary" 2>/dev/null | grep -q NEEDED; then
    echo "ERROR: $name has DT_NEEDED" >&2
    exit 1
  fi
done
[ "$found" -eq 1 ] || { echo "ERROR: no executable APEX binaries in $dir" >&2; exit 1; }
for name in matonos-flatpak matonos-bwrap matonos-app-exec; do
  [ -x "$dir/$name" ] || { echo "ERROR: missing staged binary $name" >&2; exit 1; }
done
echo "PASS staged APEX ELF checks: $dir"

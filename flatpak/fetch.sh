#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE=${CACHE_DIR:-$HERE/cache}
mkdir -p "$CACHE/git" "$CACHE/tar" "$CACHE/src"
LOCAL_REPOS=${LOCAL_REPOS:-$HOME/matonos/repos}
while IFS='|' read -r name kind location revision digest; do
  case "$name" in ''|'#'*) continue;; esac
  case "$kind" in
    git|git-local)
      dest=$CACHE/git/$name
      if [ ! -d "$dest/.git" ]; then
        case "$kind" in
          git-local) source=$LOCAL_REPOS/DullPGP ;;
          *) source=$location ;;
        esac
        git clone --no-checkout "$source" "$dest"
      fi
      git -C "$dest" fetch --no-tags origin "$revision" 2>/dev/null || :
      git -C "$dest" cat-file -e "$revision^{commit}" || { echo "Missing locked commit $name $revision" >&2; exit 1; }
      actual=$(git -C "$dest" rev-parse "$revision^{commit}")
      case "$actual" in "$revision"*) ;; *) echo "Commit mismatch for $name: $actual != $revision" >&2; exit 1;; esac
      printf '%s\t%s\t%s\n' "$name" "$actual" "$location" > "$CACHE/src/$name.pin"
      ;;
    tar|deb)
      file=${location##*/}
      case "$file" in download) file=$name.tar.gz;; esac
      [ -s "$CACHE/tar/$file" ] || curl -fL "$location" -o "$CACHE/tar/$file"
      printf '%s  %s\n' "$digest" "$CACHE/tar/$file" | sha256sum -c -
      printf '%s\t%s\t%s\n' "$name" "$revision" "$digest" > "$CACHE/src/$name.pin"
      ;;
    *) echo "Unknown lock type: $kind" >&2; exit 2;;
  esac
done < "$HERE/sources.lock"

# Expand the pinned repositories into a source tree. Submodules used by the
# spike are explicit lock entries and checked out at their recorded commits.
for name in glib ostree flatpak bionic-fill DullPGP; do
  dest=$CACHE/src/$name
  rm -rf "$dest"
  mkdir -p "$dest"
  rev=$(cut -f2 "$CACHE/src/$name.pin")
  git -C "$CACHE/git/$name" archive "$rev" | tar -x -C "$dest"
done
for pair in 'glib-gvdb:glib/subprojects/gvdb' 'ostree-libglnx:ostree/libglnx' 'ostree-bsdiff:ostree/bsdiff' 'flatpak-libglnx:flatpak/subprojects/libglnx' 'variant-schema-compiler:flatpak/subprojects/variant-schema-compiler'; do
  name=${pair%%:*}; rel=${pair#*:}; rev=$(cut -f2 "$CACHE/src/$name.pin")
  mkdir -p "$CACHE/src/$rel"
  git -C "$CACHE/git/$name" archive "$rev" | tar -x -C "$CACHE/src/$rel"
done
cp "$CACHE/src/flatpak/subprojects/packagefiles/variant-schema-compiler/meson.build" \
  "$CACHE/src/flatpak/subprojects/variant-schema-compiler/meson.build"
while IFS='|' read -r name kind location revision digest; do
  [ "$kind" = tar ] || continue
  archive_name=${location##*/}; case "$archive_name" in download) archive_name=$name.tar.gz;; esac
  archive=$CACHE/tar/$archive_name; dest=$CACHE/src/$name
  rm -rf "$dest"; mkdir -p "$dest"
  tar -xf "$archive" -C "$dest" --strip-components=1
done < "$HERE/sources.lock"
while IFS='|' read -r name kind location revision digest; do
  [ "$kind" = deb ] || continue
  archive=$CACHE/tar/${location##*/}
  dest=$CACHE/src/$name
  mkdir -p "$dest"
  dpkg-deb -x "$archive" "$dest"
done < "$HERE/sources.lock"
cp "$HERE/gpgme.m4" "$CACHE/src/DullPGP/gpgme.m4"
cp "$CACHE/src/bionic-fill/bionic-fill.h" "$CACHE/src/bionic-fill.h"
mkdir -p "$CACHE/src/meson"
tar -xf "$CACHE/tar/meson-1.12.1.tar.gz" -C "$CACHE/src/meson" --strip-components=1
printf 'Fetched and verified locked sources into %s\n' "$CACHE/src"

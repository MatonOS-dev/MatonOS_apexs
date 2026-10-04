#!/bin/sh
# Refresh signed Alpine 3.24 stable APKs and their corresponding source offer.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=${ARCH:-$(uname -m)}
BASE=https://dl-cdn.alpinelinux.org/alpine
RELEASE=$BASE/v3.24/releases/$ARCH
REPO=$BASE/v3.24
SCRATCH=${SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
ROOTFS=$SCRATCH/fetch-rootfs
APKDIR=$HERE/alpine-bin/$ARCH
SRCDIR=$HERE/alpine-src
APORTS_COMMIT=${APORTS_COMMIT:-$(git ls-remote https://gitlab.alpinelinux.org/alpine/aports.git refs/heads/3.24-stable | awk 'NR==1 {print $1}')}
[ -n "$APORTS_COMMIT" ] || { echo 'Could not resolve aports 3.24-stable commit' >&2; exit 1; }
mkdir -p "$SCRATCH" "$APKDIR" "$SRCDIR"

MINI=$(curl -fsSL "$RELEASE/" | sed -n 's/.*href="\(alpine-minirootfs-3\.24\.[0-9][0-9]*-'"$ARCH"'\.tar\.gz\)".*/\1/p' | sort -V | tail -1)
[ -n "$MINI" ] || { echo "No Alpine 3.24 minirootfs for $ARCH" >&2; exit 1; }
curl -fL "$RELEASE/$MINI" -o "$SCRATCH/$MINI"
MINI_SHA=$(sha256sum "$SCRATCH/$MINI" | awk '{print $1}')
MINI_MODIFIED=$(curl -fsSI "$RELEASE/$MINI" | awk 'tolower($1)=="last-modified:" {sub(/^[^:]*:[[:space:]]*/, ""); sub(/\r$/, ""); print; exit}')
rm -rf "$ROOTFS" && mkdir -p "$ROOTFS/apkout"
tar -xzf "$SCRATCH/$MINI" -C "$ROOTFS"
printf '%s/main\n%s/community\n' "$REPO" "$REPO" > "$ROOTFS/etc/apk/repositories"

# apk update verifies the stable indexes against keys shipped in this minirootfs.
inroot() {
  /usr/bin/bwrap --bind "$ROOTFS" / --bind "$APKDIR" /apkout \
    --proc /proc --dev /dev --tmpfs /tmp \
    --ro-bind /etc/resolv.conf /etc/resolv.conf --ro-bind /etc/hosts /etc/hosts \
    --setenv PATH /sbin:/usr/sbin:/bin:/usr/bin -- "$@"
}
inroot /sbin/apk update

# Keep this architecture's repository limited to the newly resolved closure.
find "$APKDIR" -maxdepth 1 -type f -name '*.apk' -delete

set --
while IFS= read -r pkg; do
  case "$pkg" in ''|'#'*) continue;; esac
  set -- "$@" "$pkg"
done < "$HERE/packages.txt"
inroot /sbin/apk fetch --recursive --output /apkout "$@"

# Record the complete closure by APK metadata and content digest.
: > "$HERE/apk.lock.tmp"
for apk in "$APKDIR"/*.apk; do
  [ -f "$apk" ] || continue
  meta=$(tar --warning=no-unknown-keyword -xzOf "$apk" .PKGINFO)
  name=$(printf '%s\n' "$meta" | sed -n 's/^pkgname = //p' | head -1)
  ver=$(printf '%s\n' "$meta" | sed -n 's/^pkgver = //p' | head -1)
  sha=$(sha256sum "$apk" | awk '{print $1}')
  printf '%s %s %s %s\n' "$name" "$ver" "$sha" "$(basename "$apk")" >> "$HERE/apk.lock.tmp"
done
LC_ALL=C sort -k1,1 "$HERE/apk.lock.tmp" > "$HERE/apk.lock"
rm -f "$HERE/apk.lock.tmp"
if awk '$1 == "openssl-libs-static" {found=1} END {exit !found}' "$HERE/apk.lock"; then
  echo 'Forbidden openssl-libs-static resolved from Alpine package inputs' >&2; exit 1
fi

APORTS=$SCRATCH/aports
if [ ! -d "$APORTS/.git" ]; then git clone --filter=blob:none https://gitlab.alpinelinux.org/alpine/aports.git "$APORTS"; fi
git -C "$APORTS" fetch --depth=1 origin "$APORTS_COMMIT"
git -C "$APORTS" checkout --detach FETCH_HEAD

# Replace stale source offers on refresh. BoringSSL and Flatpak's additional
# fork patch are separately pinned and kept.
if [ "${KEEP_SOURCES:-0}" != 1 ]; then
  for old_source in "$SRCDIR"/*; do
    [ -e "$old_source" ] || continue
    [ "$(basename "$old_source")" = boringssl ] && continue
    if [ "$(basename "$old_source")" = flatpak ]; then
      find "$old_source" -mindepth 1 -maxdepth 1 ! -name flatpak-trim.patch -exec rm -rf '{}' \;
      continue
    fi
    rm -rf "$old_source"
  done
fi

# A source recipe is retained when we build it or link code from its -static
# or -dev package. .PKGINFO origin maps split packages to their source recipe.
needs_source() {
  case "$1" in *-static|*-dev|flatpak|ostree|bubblewrap|curl|libarchive|json-glib|libxmlb) return 0;; esac
  return 1
}
fetch_source() {
  origin=$1
  dest=$SRCDIR/$origin
  [ "$(cat "$dest/.complete" 2>/dev/null || true)" = "$APORTS_COMMIT" ] && return 0
  build=$(find "$APORTS/main" "$APORTS/community" "$APORTS/testing" -mindepth 2 -maxdepth 2 -type f -path "*/$origin/APKBUILD" -print -quit 2>/dev/null || true)
  [ -n "$build" ] || { echo "No APKBUILD at pinned aports commit for $origin" >&2; return 1; }
  mkdir -p "$dest" "$ROOTFS/var/tmp"
  cp "$build" "$dest/APKBUILD"
  cp "$build" "$ROOTFS/var/tmp/$origin.APKBUILD"
  pkgdir=$(dirname "$build")
  find "$pkgdir" -maxdepth 1 -type f \( -name '*.patch' -o -name '*.conf' \) -exec cp '{}' "$dest/" \;
  inroot /bin/ash -c '. "$1"; printf "%s\\n" "${source-}"' sh "/var/tmp/$origin.APKBUILD" > "$SCRATCH/$origin.sources"
  inroot /bin/ash -c '. "$1"; printf "%s\\n" "${sha512sums-}"' sh "/var/tmp/$origin.APKBUILD" > "$SCRATCH/$origin.sums"
  sed '/^[[:space:]]*$/d; s/^[[:space:]]*//; s/[[:space:]]*$//' "$SCRATCH/$origin.sources" > "$SCRATCH/$origin.sources.parsed"
  awk 'NF>=2 && length($1)==128 && $1 ~ /^[0-9a-fA-F]+$/ {print}' "$SCRATCH/$origin.sums" > "$SCRATCH/$origin.sums.parsed"
  paste -d '|' "$SCRATCH/$origin.sources.parsed" "$SCRATCH/$origin.sums.parsed" |
  while IFS='|' read -r item sumrow; do
    [ -n "$item" ] || continue
    expected=${sumrow%% *}; declared=${sumrow#*  }
    [ "$declared" != "$sumrow" ] || { echo "Malformed source/checksum pair in $origin" >&2; exit 1; }
    case "$item" in *::*) file=${item%%::*}; url=${item#*::};; *) url=$item; file=$declared;; esac
    [ "$file" = "$declared" ] || { echo "APKBUILD source/checksum mismatch: $origin $file $declared" >&2; exit 1; }
    case "$url" in http://*|https://*) ;; *)
      if [ -f "$pkgdir/$url" ]; then cp "$pkgdir/$url" "$dest/$file"; fi
      url="https://distfiles.alpinelinux.org/distfiles/v3.24/${url##*/}";;
    esac
    if [ ! -f "$dest/$file" ] || ! printf '%s  %s\n' "$expected" "$dest/$file" | sha512sum -c - >/dev/null 2>&1; then
      rm -f "$dest/$file"
      curl -fLsS "$url" -o "$dest/$file" || curl -fLsS "https://distfiles.alpinelinux.org/distfiles/v3.24/${file##*/}" -o "$dest/$file"
    fi
    printf '%s  %s\n' "$expected" "$dest/$file" | sha512sum -c -
  done
  printf '%s\n' "$APORTS_COMMIT" > "$dest/.complete"
}

for apk in "$APKDIR"/*.apk; do
  [ -f "$apk" ] || continue
  meta=$(tar --warning=no-unknown-keyword -xzOf "$apk" .PKGINFO)
  name=$(printf '%s\n' "$meta" | sed -n 's/^pkgname = //p' | head -1)
  origin=$(printf '%s\n' "$meta" | sed -n 's/^origin = //p' | head -1)
  [ -n "$origin" ] || origin=$name
  if needs_source "$name"; then fetch_source "$origin"; fi
done
while IFS= read -r origin; do
  case "$origin" in ''|'#'*) continue;; esac
  fetch_source "$origin"
done < "$HERE/sources.txt"

cat > "$HERE/alpine.lock" <<LOCK
architecture=$ARCH
minirootfs_url=$RELEASE/$MINI
minirootfs_last_modified=$MINI_MODIFIED
minirootfs_sha256=$MINI_SHA
aports_branch=3.24-stable
aports_commit=$APORTS_COMMIT
flatpak_fork_commit=03e6b205d01c9560a829941d95065a7c683d9667
repository=$REPO
LOCK

# Prepare the build root entirely from the retained minirootfs and local APKs.
BUILDROOT=$SCRATCH/rootfs/$ARCH
LOCALREPO=$SCRATCH/local-repo/$ARCH
rm -rf "$BUILDROOT" "$SCRATCH/local-repo"
NOARCHREPO=$SCRATCH/local-repo/noarch
mkdir -p "$BUILDROOT" "$LOCALREPO" "$NOARCHREPO"
tar -xzf "$SCRATCH/$MINI" -C "$BUILDROOT"
for apk in "$APKDIR"/*.apk; do
  arch=$(tar --warning=no-unknown-keyword -xzOf "$apk" .PKGINFO | sed -n 's/^arch = //p' | head -1)
  if [ "$arch" = noarch ]; then cp "$apk" "$NOARCHREPO/"; else cp "$apk" "$LOCALREPO/"; fi
done
mkdir -p "$BUILDROOT/local-repo"
inbuildroot() {
  /usr/bin/bwrap --bind "$BUILDROOT" / --bind "$SCRATCH/local-repo" /repo \
    --proc /proc --dev /dev --tmpfs /tmp \
    --setenv PATH /sbin:/usr/sbin:/bin:/usr/bin --setenv ARCH "$ARCH" -- "$@"
}
inbuildroot /bin/sh -c \
  "apk index --description 'MatonOS locked Alpine 3.24 packages' --output /repo/$ARCH/APKINDEX.tar.gz /repo/$ARCH/*.apk /repo/noarch/*.apk"
inbuildroot /sbin/apk update --allow-untrusted --no-network \
  --repositories-file /dev/null --repository /repo
inbuildroot /sbin/apk upgrade --available --no-network --allow-untrusted \
  --repositories-file /dev/null --repository /repo
LOCKED=$(awk '{print $1 "=" $2}' "$HERE/apk.lock" | tr '\n' ' ')
inbuildroot /sbin/apk add --no-network --allow-untrusted \
  --repositories-file /dev/null --repository /repo $LOCKED

echo "APK count: $(wc -l < "$HERE/apk.lock")"
echo "APK bytes: $(du -sb "$APKDIR" | awk '{print $1}')"
echo "Source bytes: $(du -sb "$SRCDIR" | awk '{print $1}')"

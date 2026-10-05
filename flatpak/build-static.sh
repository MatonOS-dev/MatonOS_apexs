#!/bin/sh
# Reproducible static-musl spike. Run inside the pinned Alpine root with:
# /work/alpine-enter.sh /work/build-static.sh
set -eu
WORK=/work SCRATCH=/scratch SRC=/work/alpine-src
BUILD=${BUILD:-$SCRATCH/build}
OUT=/scratch/output
JOBS=${JOBS:-16}
ARCH=${ARCH:-$(uname -m)}
SYSTEM_BWRAP=${SYSTEM_BWRAP:-/apex/com.matonos.flatpak/bin/matonos-bwrap}
SYSTEM_DBUS_PROXY=${SYSTEM_DBUS_PROXY:-/apex/com.matonos.flatpak/bin/xdg-dbus-proxy}
[ "$JOBS" -le 16 ] || { echo 'JOBS must be <= 16' >&2; exit 2; }
mkdir -p "$BUILD" "$OUT" /usr/local/lib/pkgconfig
rm -f "$OUT/gpg"
cd "$SRC"
# Offline-only package install. apk.lock is SHA-256 checked before apk sees
# these files; the generated local index provides dependency resolution.
APKDIR="$WORK/alpine-bin/$ARCH"
test -s "$WORK/apk.lock" || { echo 'Missing apk.lock; run fetch-apks.sh first' >&2; exit 1; }
if awk '$1 == "openssl-libs-static" {bad=1} END {exit !bad}' "$WORK/apk.lock"; then
  echo 'ERROR: openssl-libs-static is forbidden in apk.lock' >&2
  exit 1
fi
APKREPO="$BUILD/apk-repo/$ARCH"
mkdir -p "$APKREPO" "$BUILD/apk-repo/noarch"
while read -r name version sha file; do
  [ -n "$name" ] || continue
  [ -f "$APKDIR/$file" ] || { echo "Missing locked APK $file" >&2; exit 1; }
  printf '%s  %s\n' "$sha" "$APKDIR/$file" | sha256sum -c -
  apkarch=$(tar -xzOf "$APKDIR/$file" .PKGINFO | sed -n 's/^arch = //p' | head -1)
  if [ "$apkarch" = noarch ]; then
    ln -sf "$APKDIR/$file" "$BUILD/apk-repo/noarch/$file"
  else
    ln -sf "$APKDIR/$file" "$APKREPO/$file"
  fi
done < "$WORK/apk.lock"
apk index --description 'MatonOS offline locked Alpine packages' \
  --output "$APKREPO/APKINDEX.tar.gz" "$APKREPO"/*.apk "$BUILD/apk-repo/noarch"/*.apk
apk update --allow-untrusted --no-network \
  --repositories-file /dev/null --repository "$BUILD/apk-repo"
APK_PACKAGES=$(awk '{print $1 "=" $2}' "$WORK/apk.lock" | tr '\n' ' ')
apk add --no-network --allow-untrusted --repositories-file /dev/null \
  --repository "$BUILD/apk-repo" $APK_PACKAGES
export CFLAGS='-Os -ffunction-sections -fdata-sections -I/usr/local/include/gpgme-lite' CXXFLAGS='-Os -ffunction-sections -fdata-sections'
export LDFLAGS='-Wl,--gc-sections'
export PATH=/usr/local/bin:$PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG='pkg-config --static'
if ! grep -q '^Libs.private:.*-leconf' /usr/lib/pkgconfig/blkid.pc; then printf '\nLibs.private: -leconf\n' >> /usr/lib/pkgconfig/blkid.pc; fi
unpack() {
  package=$1; preferred=$2
  archive=$SRC/$preferred
  if [ ! -f "$archive" ]; then
    archive=$(find "$WORK/alpine-src/$package" -maxdepth 1 -type f \
      \( -name '*.tar.gz' -o -name '*.tar.xz' -o -name '*.tar.bz2' -o -name '*.tgz' \) \
      -print -quit 2>/dev/null || true)
  fi
  [ -n "$archive" ] && [ -f "$archive" ] || { echo "Missing offline source archive for $package" >&2; exit 1; }
  rm -rf "$BUILD/$package"; mkdir -p "$BUILD/$package"
  tar --no-same-owner -xf "$archive" -C "$BUILD/$package" --strip-components=1
  for source_patch in "$WORK/alpine-src/$package"/*.patch; do
    [ -f "$source_patch" ] || continue
    patch -d "$BUILD/$package" -p1 < "$source_patch"
  done
}
BORINGSSL_ARCHIVE=$WORK/alpine-src/boringssl/boringssl.tar.gz
BORINGSSL_ARCHIVE_SHA=$(sed -n 's/^archive-gzip-sha256=//p' "$WORK/boringssl.lock")
printf '%s  %s\n' "$BORINGSSL_ARCHIVE_SHA" "$BORINGSSL_ARCHIVE" | sha256sum -c -
BORINGSSL_TREE_SHA=$(sed -n 's/^git-archive-sha256=//p' "$WORK/boringssl.lock")
BORINGSSL_ACTUAL_SHA=$(gzip -cd "$BORINGSSL_ARCHIVE" | sha256sum | awk '{print $1}')
[ "$BORINGSSL_ACTUAL_SHA" = "$BORINGSSL_TREE_SHA" ] || { echo 'BoringSSL source archive does not match boringssl.lock' >&2; exit 1; }
BORINGSSL_SRC=${BORINGSSL_SRC:-$BUILD/boringssl-src}
BORINGSSL_COMMIT=0cd1f6a94b670e6b61af95a06e71e690ae8e3262
if [ ! -f "$BORINGSSL_SRC/CMakeLists.txt" ]; then
  rm -rf "$BORINGSSL_SRC"; mkdir -p "$BORINGSSL_SRC"
  tar -xzf "$BORINGSSL_ARCHIVE" -C "$BORINGSSL_SRC"
fi
cmake -S "$BORINGSSL_SRC" -B "$BUILD/boringssl" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON
ninja -C "$BUILD/boringssl" -j "$JOBS" crypto ssl
install -D -m644 "$BUILD/boringssl/crypto/libcrypto.a" /usr/local/lib/libcrypto.a 2>/dev/null || \
  install -D -m644 "$BUILD/boringssl/libcrypto.a" /usr/local/lib/libcrypto.a
install -D -m644 "$BUILD/boringssl/ssl/libssl.a" /usr/local/lib/libssl.a 2>/dev/null || \
  install -D -m644 "$BUILD/boringssl/libssl.a" /usr/local/lib/libssl.a
mkdir -p /usr/local/include/openssl
cp -R "$BORINGSSL_SRC/include/openssl/." /usr/local/include/openssl/
# Alpine lacks static json-glib and libxmlb archives.
unpack json-glib json-glib-1.10.8.tar.xz
meson setup "$BUILD/json-glib/out" "$BUILD/json-glib" --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dman=false -Dintrospection=disabled -Dtests=false
ninja -C "$BUILD/json-glib/out" -j "$JOBS" install
unpack libxmlb libxmlb-0.3.27.tar.gz
meson setup "$BUILD/libxmlb/out" "$BUILD/libxmlb" --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dcli=false -Dintrospection=false -Dgtkdoc=false -Dtests=false
ninja -C "$BUILD/libxmlb/out" -j "$JOBS" install
# Alpine 3.24 stable bubblewrap 0.12.0.
unpack bubblewrap bubblewrap-0.12.0.tar.gz
meson setup "$BUILD/bubblewrap/out" "$BUILD/bubblewrap" --prefix=/usr/local --default-library=static --prefer-static -Dman=disabled -Dtests=false -Dc_args="$CFLAGS" -Dc_link_args="-static $LDFLAGS"
ninja -C "$BUILD/bubblewrap/out" -v -j "$JOBS"
install -D -m755 "$BUILD/bubblewrap/out/bwrap" "$OUT/bwrap"
install -D -m755 "$BUILD/bubblewrap/out/bwrap" /usr/bin/bwrap
# GPGME_LITE=1 is the default and only verifier implementation. Build a copy
# of the selected local source tree inside scratch so Make never writes there.
GPGME_LITE=${GPGME_LITE:-1}
[ "$GPGME_LITE" = 1 ] || { echo 'Only GPGME_LITE=1 is supported' >&2; exit 2; }
rm -rf "$BUILD/gpgme-lite-src"
cp -R /work-gpgme-lite "$BUILD/gpgme-lite-src"
make -C "$BUILD/gpgme-lite-src" clean
make -C "$BUILD/gpgme-lite-src" -j"$JOBS" \
  CFLAGS="$CFLAGS" BSSL_SRC="$BORINGSSL_SRC" BSSL_LIBDIR=/usr/local/lib \
  PREFIX=/usr/local LIBDIR=/usr/local/lib INCLUDEDIR=/usr/local/include
install -D -m644 "$BUILD/gpgme-lite-src/libgpgme-lite.a" /usr/local/lib/libgpgme-lite.a
install -D -m644 "$BUILD/gpgme-lite-src/include/gpgme.h" /usr/local/include/gpgme-lite/gpgme.h
install -D -m644 "$BUILD/gpgme-lite-src/include/gpg-error.h" /usr/local/include/gpgme-lite/gpg-error.h
cat > /usr/local/lib/pkgconfig/gpgme.pc <<'GPGME_PC'
prefix=/usr/local
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include/gpgme-lite

Name: gpgme-lite
Description: Minimal GPGME-compatible OpenPGP verification library
Version: 2.0.1
Libs: -L${libdir} -lgpgme-lite
Libs.private: -L${libdir} -lcrypto
Cflags: -I${includedir}
GPGME_PC
cat > /usr/local/lib/pkgconfig/gpg-error.pc <<'GPG_ERROR_PC'
prefix=/usr/local
includedir=${prefix}/include/gpgme-lite

Name: gpg-error-lite
Description: GPG error compatibility definitions for gpgme-lite
Version: 1.61
Libs:
Cflags: -I${includedir}
GPG_ERROR_PC
# Build curl from the pinned Alpine 3.24 source against BoringSSL. This binary
# stack exposes only HTTP/HTTPS and uses Android's configurable CA directory.
CURL_CAPATH=${CURL_CAPATH:-/apex/com.android.conscrypt/cacerts}
unpack curl curl-8.22.0.tar.xz
(cd "$BUILD/curl" && ./configure --prefix=/usr/local --enable-static --disable-shared \
  --with-openssl=/usr/local --with-ca-path="$CURL_CAPATH" \
  --disable-ftp --enable-file --disable-websockets --disable-ldap --disable-ldaps --disable-rtsp \
  --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
  --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-ipfs \
  --without-libssh2 --without-librtmp --without-nghttp2 --without-libidn2 \
  --without-libpsl --without-brotli \
  CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" LIBS='-lstdc++ -pthread' && make -j"$JOBS" && make install)
PROTO_LIST=$(curl --version | sed -n 's/^Protocols: //p' | tr '[:lower:]' '[:upper:]' | tr ' ' '\n' | sort | paste -sd ' ' -)
[ "$PROTO_LIST" = 'FILE HTTP HTTPS' ] || { echo "Unexpected curl protocols: $PROTO_LIST" >&2; exit 1; }
# Rebuild libarchive without any crypto backend so it cannot pull OpenSSL.
unpack libarchive libarchive-3.8.7.tar.xz
(cd "$BUILD/libarchive" && ./configure --prefix=/usr/local --enable-static --disable-shared \
  --without-openssl --without-nettle --without-mbedtls --without-lzo2 \
  CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" && make -j"$JOBS" && make install)
# OSTree 2025.7 uses the private curl build and DullPGP verifier.
unpack ostree libostree-2025.7.tar.xz
patch -d "$BUILD/ostree" -p1 < "$WORK/ostree-flatpak-user-agent.patch"
(cd "$BUILD/ostree" && NOCONFIGURE=1 ./autogen.sh && PKG_CONFIG="$PKG_CONFIG" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" ./configure --prefix=/usr/local --libdir=/usr/local/lib --enable-static --disable-shared --with-curl --without-soup --with-libarchive --with-gpgme --disable-gtk-doc && make libglnx-config.h && make -j"$JOBS" V=1 CFLAGS="$CFLAGS $($PKG_CONFIG --cflags gpgme gpg-error)" LDFLAGS="-all-static $LDFLAGS" ostree && make -j"$JOBS" install-libLTLIBRARIES install-pkgconfigDATA install-libostreeincludeHEADERS)
install -D -m755 "$BUILD/ostree/ostree" "$OUT/ostree"
readelf -l "$OUT/ostree" | grep -q INTERP && { echo 'BLOCKED: OSTree still has an ELF interpreter' >&2; exit 3; } || :
readelf -d "$OUT/ostree" 2>/dev/null | grep -q NEEDED && { echo 'BLOCKED: OSTree still needs shared libraries' >&2; exit 3; } || :
# Meson requires configured external helper paths to exist at configure time.
# Put a regular
# build-only bwrap file at the final shim path; the device tree stages its own
# shim there, which dispatches the multicall ELF with argv[0]=bwrap.
mkdir -p /apex/com.matonos.flatpak/bin
install -D -m755 /usr/bin/bwrap /apex/com.matonos.flatpak/bin/matonos-bwrap
rm -f /apex/com.matonos.flatpak/bin/xdg-dbus-proxy
cat > /apex/com.matonos.flatpak/bin/xdg-dbus-proxy <<'PROXY'
#!/bin/sh
printf 'xdg-dbus-proxy 0.1.7\n'
PROXY
chmod 755 /apex/com.matonos.flatpak/bin/xdg-dbus-proxy
FLATPAK_REPO=${FLATPAK_REPO:-/scratch/flatpak-work}
FLATPAK_COMMIT=${FLATPAK_COMMIT:-03e6b205d01c9560a829941d95065a7c683d9667}
if [ -d "$FLATPAK_REPO/.git" ]; then
  test "$(git -C "$FLATPAK_REPO" rev-parse "$FLATPAK_COMMIT^{commit}")" = "$FLATPAK_COMMIT"
  mkdir -p "$BUILD/flatpak"
  git -C "$FLATPAK_REPO" archive "$FLATPAK_COMMIT" | tar -x -C "$BUILD/flatpak"
  patch -d "$BUILD/flatpak" -p1 < "$WORK/alpine-src/flatpak/tzdir.patch"
else
  unpack flatpak flatpak-1.16.6.tar.xz
fi
rm -rf "$BUILD/flatpak/out"
(cd "$BUILD/flatpak" && meson setup out . --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dc_args="$CFLAGS" -Dc_link_args=-static -Dtests=false -Ddbus_config_dir=/usr/share/dbus-1/system.d -Dsystem_bubblewrap="$SYSTEM_BWRAP" -Dsystem_dbus_proxy="$SYSTEM_DBUS_PROXY" -Dgdm_env_file=true -Dmalcontent=disabled -Dsystem_helper=disabled -Dsystemd=disabled -Dgir=disabled -Dgtkdoc=disabled -Ddocbook_docs=disabled -Dhttp_backend=curl -Dman=disabled -Ddconf=disabled -Dxauth=disabled -Dauto_sideloading=false && ninja -C out -j"$JOBS")
# Build each CLI into one relocatable object. Keep libostree, libotutil,
# libotcore and the selected libglnx out of these objects so they are included
# exactly once in the final image. Flatpak's app/common archives and OSTree's
# bsdiff archive are private to their respective CLI.
MULTI="$BUILD/multicall"
mkdir -p "$MULTI"
cd "$BUILD/flatpak/out"
ld -r -o "$MULTI/flatpak.o" app/flatpak.p/*.o \
  --start-group app/liblibflatpak-app.a common/*.a --end-group
cd "$BUILD/ostree"
ld -r -o "$MULTI/ostree.o" src/ostree/*.o --start-group .libs/libbsdiff.a --end-group
cd "$BUILD/bubblewrap/out"
ld -r -o "$MULTI/bwrap.o" bwrap.p/*.o
for tool in flatpak ostree bwrap; do
  if [ "$tool" = ostree ]; then
    # OSTree also has an internal helper with the name ostree_main.
    objcopy --redefine-sym=ostree_main=ostree_builtin_main "$MULTI/$tool.o"
  fi
  objcopy --redefine-sym=main=${tool}_main "$MULTI/$tool.o"
  objcopy --keep-global-symbol=${tool}_main "$MULTI/$tool.o"
done
cc -Os -ffunction-sections -fdata-sections -c "$WORK/multicall.c" -o "$MULTI/multicall.o"
# Flatpak's vendored libglnx is newer than OSTree's copy and contains the
# common API used by both CLIs. The two archives export dozens of identically
# named functions, so use the Flatpak archive once in the final static link.
STATIC_LIBS=$($PKG_CONFIG --libs glib-2.0 gio-unix-2.0 json-glib-1.0 libarchive libseccomp libcap libxml-2.0 gpgme libcurl liblzma ostree-1)
STATIC_LIBS=$(printf '%s\n' "$STATIC_LIBS" | sed 's/-lostree-1//g')
STATIC_LIBS="$STATIC_LIBS -lstdc++"
cc -Os -static-pie -Wl,--gc-sections,-Map="$BUILD/matonos-flatpak.map" -o "$OUT/matonos-flatpak.unstripped" \
  "$MULTI/multicall.o" "$MULTI/flatpak.o" "$MULTI/ostree.o" "$MULTI/bwrap.o" \
  -Wl,--start-group \
  "$BUILD/ostree/.libs/libostree-1.a" "$BUILD/ostree/.libs/libotutil.a" \
  "$BUILD/ostree/.libs/libotcore.a" "$BUILD/flatpak/out/subprojects/libglnx/libglnx.a" \
  $STATIC_LIBS -Wl,--end-group
if awk '/^\/usr\/local\/lib\/libcrypto\.a\(/ {s=$0; sub(/^.*\(/,"",s); sub(/\).*/,"",s); print s}' \
    "$BUILD/matonos-flatpak.map" | sort | uniq -d | grep .; then
  echo 'ERROR: a BoringSSL archive member was linked more than once' >&2
  exit 1
fi
if nm "$OUT/matonos-flatpak.unstripped" 2>/dev/null | grep -E '(^|[[:space:]])_gpgme_|(^|[[:space:]])assuan_'; then
  echo 'ERROR: real GPGME/libassuan symbols remain in matonos-flatpak' >&2
  exit 1
fi
mv "$OUT/matonos-flatpak.unstripped" "$OUT/matonos-flatpak"
strip --strip-all "$OUT/matonos-flatpak"
if nm "$OUT/matonos-flatpak" 2>/dev/null | grep -E 'OSSL_LIB_CTX_new|EVP_CIPHER_fetch|OPENSSL_version_(major|num)'; then
  echo 'ERROR: OpenSSL-only symbols remain in matonos-flatpak' >&2
  exit 1
fi
if strings "$OUT/matonos-flatpak" | grep -E 'OpenSSL [123]\\.[0-9]|OpenSSL version|OPENSSL_VERSION_TEXT'; then
  echo 'ERROR: OpenSSL version strings remain in matonos-flatpak' >&2
  exit 1
fi
if nm "$OUT/matonos-flatpak" 2>/dev/null | grep -E 'gdk_pixbuf_|as_metadata_'; then
  echo 'ERROR: Flatpak contains unwanted GdkPixbuf/AppStream symbols' >&2
  exit 1
fi
if strings "$OUT/matonos-flatpak" | grep -E 'gdk_pixbuf_|as_metadata_'; then
  echo 'ERROR: Flatpak contains unwanted GdkPixbuf/AppStream strings' >&2
  exit 1
fi
bin_list='matonos-flatpak'
for bin in $bin_list; do
  test -x "$OUT/$bin"
  file "$OUT/$bin"
  readelf -l "$OUT/$bin" | grep -q INTERP && { echo "ERROR: $bin has INTERP" >&2; exit 1; } || :
  readelf -d "$OUT/$bin" 2>/dev/null | grep -q NEEDED && { echo "ERROR: $bin has NEEDED" >&2; exit 1; } || :
  printf '%s %s bytes\n' "$bin" "$(stat -c %s "$OUT/$bin")"
done
cc -O2 -Wall -Wextra -Werror "$WORK/applet-exec.c" -o "$BUILD/applet-exec"
"$BUILD/applet-exec" "$OUT/matonos-flatpak" flatpak --version
"$BUILD/applet-exec" "$OUT/matonos-flatpak" flatpak remote-ls --help >/dev/null
"$BUILD/applet-exec" "$OUT/matonos-flatpak" ostree --version
"$BUILD/applet-exec" "$OUT/matonos-flatpak" bwrap --version
"$BUILD/applet-exec" "$OUT/matonos-flatpak" matonos-flatpak build-update-repo --help >/dev/null
if "$BUILD/applet-exec" "$OUT/matonos-flatpak" unknown-tool >/dev/null 2>&1; then
  echo 'ERROR: unknown multicall tool was accepted' >&2
  exit 1
else
  status=$?
  [ "$status" -eq 2 ] || { echo "ERROR: unknown applet returned $status, expected 2" >&2; exit 1; }
fi

#!/bin/sh
# Reproducible static-musl spike. Run inside the pinned Alpine root with:
# /work/alpine-enter.sh /work/build-static.sh
set -eu
WORK=/work SCRATCH=/scratch SRC=/scratch/sources
BUILD=${BUILD:-$SCRATCH/build}
GPGME_LITE=${GPGME_LITE:-0}
OUT=/scratch/output
if [ "$GPGME_LITE" = 1 ]; then OUT=/scratch/output-gpgme-lite; fi
JOBS=${JOBS:-4}
SYSTEM_BWRAP=${SYSTEM_BWRAP:-/apex/com.matonos.flatpak/bin/bwrap}
SYSTEM_DBUS_PROXY=${SYSTEM_DBUS_PROXY:-/apex/com.matonos.flatpak/bin/xdg-dbus-proxy}
[ "$JOBS" -le 4 ] || { echo 'JOBS must be <= 4' >&2; exit 2; }
mkdir -p "$BUILD" "$OUT" /usr/local/lib/pkgconfig
cd "$SRC"
grep -v '^#' "$WORK/source-lock.sha256" | sha256sum -c -
# Alpine 3.24 static subpackages used by bwrap, OSTree, and GnuPG.
APK_PACKAGES="acl-static=2.3.2-r1 expat-static=2.8.5-r0 \
  lz4-static=1.10.0-r1 libunistring-static=1.4.2-r0 \
  libgcrypt-dev=1.12.2-r0 libgcrypt-static=1.12.2-r0 \
  libassuan-dev=3.0.2-r0 libassuan-static=3.0.2-r0 \
  libgpg-error-dev=1.61-r0 libgpg-error-static=1.61-r0 gpgme-dev=2.0.1-r1 \
  libusb-dev=1.0.30-r0 zlib-static=1.3.2-r0 bzip2-dev=1.0.8-r6 bzip2-static=1.0.8-r6 \
  readline-static=8.3.3-r1 ncurses-static=6.6_p20260516-r0 brotli-static=1.2.0-r1 \
  libpsl-static=0.21.5-r3 libidn2-static=2.3.8-r0 \
  nghttp2-static=1.70.0-r0 libgcc-static=15.2.0-r5 \
  build-base=0.5-r4 autoconf=2.73-r0 automake=1.18.1-r1 libtool=2.6.0-r1 \
  bison=3.8.2-r3 gettext-dev=1.0-r0 pkgconf=2.5.1-r0 meson=1.11.1-r0 samurai=1.2-r8 \
  glib-dev=2.88.1-r1 glib-static=2.88.1-r1 libffi-dev=3.5.2-r1 \
  curl-dev=8.22.0-r0 curl-static=8.22.0-r0 libarchive-dev=3.8.7-r0 libarchive-static=3.8.7-r0 \
  libxml2-dev=2.13.9-r2 libxml2-static=2.13.9-r2 openssl-dev=3.5.9-r0 \
  openssl-libs-static=3.5.9-r0 zstd-dev=1.5.7-r2 zstd-static=1.5.7-r2 xz-dev=5.8.4-r0 xz-static=5.8.4-r0 \
  pcre2-dev=10.49-r0 pcre2-static=10.49-r0 util-linux-dev=2.42.3-r1 util-linux-static=2.42.3-r1 \
  fuse3-dev=3.18.3-r0 fuse3-static=3.18.3-r0 libcap-dev=2.78-r0 libcap-static=2.78-r0 \
  libseccomp-dev=2.6.0-r2 libseccomp-static=2.6.0-r2 libeconf-dev=0.8.3-r0 libeconf-static=0.8.3-r0 \
  shared-mime-info=2.4-r7 gperf=3.3-r0 itstool=2.0.7-r3 \
  fuse3=3.18.3-r0"
# The supplied rootfs already contains these pinned packages. Avoid asking apk
# to rewrite its world database when every exact version is present.
if ! apk info -e $APK_PACKAGES >/dev/null 2>&1; then
  apk add --no-cache $APK_PACKAGES
fi
export CFLAGS='-Os -ffunction-sections -fdata-sections' CXXFLAGS='-Os -ffunction-sections -fdata-sections'
export LDFLAGS='-Wl,--gc-sections'
export PATH=/usr/local/bin:$PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG='pkg-config --static'
if ! grep -q '^Libs.private:.*-leconf' /usr/lib/pkgconfig/blkid.pc; then printf '\nLibs.private: -leconf\n' >> /usr/lib/pkgconfig/blkid.pc; fi
unpack() { rm -rf "$BUILD/$1"; mkdir -p "$BUILD/$1"; tar --no-same-owner -xf "$SRC/$2" -C "$BUILD/$1" --strip-components=1; }
# Alpine lacks static json-glib and libxmlb archives.
unpack json-glib json-glib-1.10.8.tar.xz
meson setup "$BUILD/json-glib/out" "$BUILD/json-glib" --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dman=false -Dintrospection=disabled -Dtests=false
ninja -C "$BUILD/json-glib/out" -j "$JOBS" install
unpack libxmlb libxmlb-0.3.27.tar.gz
meson setup "$BUILD/libxmlb/out" "$BUILD/libxmlb" --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dcli=false -Dintrospection=false -Dgtkdoc=false -Dtests=false
ninja -C "$BUILD/libxmlb/out" -j "$JOBS" install
# Bubblewrap 0.12.0, mirroring Alpine's output-static recipe.
unpack bubblewrap bubblewrap-0.12.0.tar.gz
patch -d "$BUILD/bubblewrap" -p1 < "$SRC/busybox.patch"
meson setup "$BUILD/bubblewrap/out" "$BUILD/bubblewrap" --prefix=/usr/local --default-library=static --prefer-static -Dman=disabled -Dtests=false -Dc_args="$CFLAGS" -Dc_link_args="-static $LDFLAGS"
ninja -C "$BUILD/bubblewrap/out" -v -j "$JOBS"
install -D -m755 "$BUILD/bubblewrap/out/bwrap" "$OUT/bwrap"
install -D -m755 "$BUILD/bubblewrap/out/bwrap" /usr/bin/bwrap
if [ "$GPGME_LITE" = 1 ]; then
  # OSTree and libcurl use OpenSSL; prefix the private BoringSSL copy so both
  # crypto libraries can coexist in the static multicall image.
  # Build gpgme-lite and its pinned BoringSSL dependency for Alpine musl.
  cmake -S /scratch-gpgme/boringssl-src/src -B "$BUILD/boringssl-unprefixed" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON
  ninja -C "$BUILD/boringssl-unprefixed" -j "$JOBS" crypto
  nm -g --defined-only "$BUILD/boringssl-unprefixed/libcrypto.a" | \
    awk 'NF >= 3 && $2 ~ /^[A-Z]$/ {print $3}' | sort -u \
    > /scratch-gpgme/boringssl-symbols.txt
  cmake -S /scratch-gpgme/boringssl-src/src -B "$BUILD/boringssl" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON -DBORINGSSL_PREFIX=GPGMELITE \
    -DBORINGSSL_PREFIX_SYMBOLS=/scratch-gpgme/boringssl-symbols.txt
  ninja -C "$BUILD/boringssl" boringssl_prefix_symbols
  {
    cat <<'PREFIX_HEADER'
#define GL_BSSL_ADD_PREFIX(a, b) GL_BSSL_ADD_PREFIX_INNER(a, b)
#define GL_BSSL_ADD_PREFIX_INNER(a, b) a ## _ ## b
PREFIX_HEADER
    awk '{print "#ifndef " $0; print "#define " $0 " GL_BSSL_ADD_PREFIX(BORINGSSL_PREFIX, " $0 ")"; print "#endif"}' \
      /scratch-gpgme/boringssl-symbols.txt
  } > "$BUILD/boringssl/symbol_prefix_include/boringssl_prefix_symbols.h"
  ninja -C "$BUILD/boringssl" -j "$JOBS" crypto
  install -D -m644 "$BUILD/boringssl/libcrypto.a" /usr/local/lib/gpgme-lite/libcrypto.a
  mkdir -p /usr/local/include/openssl
  cp -R /scratch-gpgme/boringssl-src/src/include/openssl/. /usr/local/include/openssl/
  install -D -m644 "$BUILD/boringssl/symbol_prefix_include/boringssl_prefix_symbols.h" /usr/local/include/boringssl_prefix_symbols.h
  mkdir -p "$BUILD/gpgme-lite-src"
  cp -R /gpgme-lite/. "$BUILD/gpgme-lite-src/"
  make -C "$BUILD/gpgme-lite-src" clean
  make -C "$BUILD/gpgme-lite-src" -j"$JOBS" GL_BORINGSSL_PREFIXED=1 BSSL_SRC=/scratch-gpgme/boringssl-src BSSL_LIBDIR=/usr/local/lib/gpgme-lite PREFIX=/usr/local LIBDIR=/usr/local/lib INCLUDEDIR=/usr/local/include
  install -D -m644 "$BUILD/gpgme-lite-src/libgpgme-lite.a" /usr/local/lib/libgpgme-lite.a
  install -D -m644 "$BUILD/gpgme-lite-src/include/gpgme.h" /usr/local/include/gpgme.h
  install -D -m644 "$BUILD/gpgme-lite-src/include/gpg-error.h" /usr/local/include/gpg-error.h
  install -D -m644 "$BUILD/gpgme-lite-src/gpgme.pc" /usr/local/lib/pkgconfig/gpgme.pc
  printf '%s\n' 'prefix=/usr/local' 'exec_prefix=${prefix}' \
    'libdir=${exec_prefix}/lib' 'includedir=${prefix}/include' '' \
    'Name: gpg-error' 'Description: Error ABI provided by gpgme-lite' \
    'Version: 1.61' 'Libs:' 'Cflags: -I${includedir}' \
    > /usr/local/lib/pkgconfig/gpg-error.pc
else
  # GPGME invokes gpg; Alpine's gnupg 2.4.9 frontend is built static.
  unpack npth npth-1.8.tar.bz2
  (cd "$BUILD/npth" && ./configure --prefix=/usr/local --enable-static --disable-shared CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" && make -j"$JOBS" && make install)
  unpack gnupg gnupg-2.4.9.tar.bz2
  (cd "$BUILD/gnupg" && for p in 0001-include-unistd.patch 0010-avoid-beta-warning.patch 0020-avoid-regenerating-defsincdate-use-shipped-file.patch 0110-avoid-simple-memory-dumps-via-ptrace.patch 0210-dirmngr-hkp-avoid-potential-race-condition-when-some-host-die.patch 0230-dirmngr-avoid-automatically-checking-upstream-swdb.patch 0330-gpg-default-to-sha512-for-all-signature-types-on-rsa-keys.patch 0340-gpg-prefer-sha512-and-sha384-in-personal-digest.patch 0420-gpg-drop-import-clean-from-default-keyserver-import-options.patch fix-i18n.patch HACK-revert-rfc4880bis-default.patch; do patch -p1 < "$SRC/$p"; done && ./configure --prefix=/usr/local --disable-doc --disable-g13 --disable-gpgsm --disable-wks-tools --disable-dirmngr --disable-card-support --disable-tofu --disable-large-secmem --disable-ldap --disable-ntbtls && make -j"$JOBS" -C kbx libkeybox.a && make -j"$JOBS" -C common libcommonpth.a libgpgrl.a && make -j"$JOBS" -C regexp libregexp.a && make -j"$JOBS" -C g10 LDFLAGS='-static -Wl,--gc-sections' LIBREADLINE='-lreadline -lncursesw' gpg)
  install -D -m755 "$BUILD/gnupg/g10/gpg" "$OUT/gpg"
fi
# OSTree 2025.7 uses curl and the GPGME verifier. This stops if Alpine does
# not provide/build a static GPGME archive; linking a shared verifier is rejected.
unpack ostree libostree-2025.7.tar.xz
(cd "$BUILD/ostree" && NOCONFIGURE=1 ./autogen.sh && PKG_CONFIG="$PKG_CONFIG" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" ./configure --prefix=/usr/local --libdir=/usr/local/lib --enable-static --disable-shared --with-curl --without-soup --with-libarchive --with-gpgme --disable-gtk-doc && make libglnx-config.h && {
  if [ "$GPGME_LITE" = 1 ]; then
    make -j"$JOBS" V=1 CFLAGS="$CFLAGS" LDFLAGS="-all-static $LDFLAGS -Wl,--whole-archive /usr/local/lib/gpgme-lite/libcrypto.a -Wl,--no-whole-archive" ostree
  else
    make -j"$JOBS" V=1 CFLAGS="$CFLAGS" LDFLAGS="-all-static $LDFLAGS" ostree
  fi
} && make -j"$JOBS" install-libLTLIBRARIES install-pkgconfigDATA install-libostreeincludeHEADERS)
install -D -m755 "$BUILD/ostree/ostree" "$OUT/ostree"
readelf -l "$OUT/ostree" | grep -q INTERP && { echo 'BLOCKED: OSTree still has an ELF interpreter' >&2; exit 3; } || :
readelf -d "$OUT/ostree" 2>/dev/null | grep -q NEEDED && { echo 'BLOCKED: OSTree still needs shared libraries' >&2; exit 3; } || :
# Meson requires configured external helper paths to exist at configure time.
# These build-root-only aliases satisfy that check; neither path is packaged.
mkdir -p /apex/com.matonos.flatpak/bin
ln -sf /usr/bin/bwrap /apex/com.matonos.flatpak/bin/bwrap
rm -f /apex/com.matonos.flatpak/bin/xdg-dbus-proxy
cat > /apex/com.matonos.flatpak/bin/xdg-dbus-proxy <<'PROXY'
#!/bin/sh
printf 'xdg-dbus-proxy 0.1.7\n'
PROXY
chmod 755 /apex/com.matonos.flatpak/bin/xdg-dbus-proxy
unpack flatpak flatpak-1.16.6.tar.xz
(cd "$BUILD/flatpak" && patch -p1 < "$SRC/tzdir.patch" && patch -p1 < "$WORK/flatpak-trim.patch" && meson setup out . --prefix=/usr/local --libdir=lib --default-library=static --prefer-static -Dc_link_args=-static -Dtests=false -Ddbus_config_dir=/usr/share/dbus-1/system.d -Dsystem_bubblewrap="$SYSTEM_BWRAP" -Dsystem_dbus_proxy="$SYSTEM_DBUS_PROXY" -Dgdm_env_file=true -Dmalcontent=disabled -Dsystem_helper=disabled -Dsystemd=disabled -Dgir=disabled -Dgtkdoc=disabled -Ddocbook_docs=disabled -Dhttp_backend=curl -Dman=disabled -Ddconf=disabled -Dxauth=disabled -Dauto_sideloading=false && ninja -C out -j"$JOBS")
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
if [ "$GPGME_LITE" = 1 ]; then
  case " $STATIC_LIBS " in
    *" -lgpg-error "*|*" -lassuan "*|*" -lgpgme ")
      echo 'ERROR: real GPGME, libgpg-error, or libassuan entered static link flags' >&2
      exit 1
      ;;
  esac
fi
cc -Os -static-pie -Wl,--gc-sections -o "$OUT/matonos-flatpak" \
  "$MULTI/multicall.o" "$MULTI/flatpak.o" "$MULTI/ostree.o" "$MULTI/bwrap.o" \
  -Wl,--start-group \
  "$BUILD/ostree/.libs/libostree-1.a" "$BUILD/ostree/.libs/libotutil.a" \
  "$BUILD/ostree/.libs/libotcore.a" "$BUILD/flatpak/out/subprojects/libglnx/libglnx.a" \
  $STATIC_LIBS -Wl,--end-group
if [ "$GPGME_LITE" = 1 ]; then
  cp "$OUT/matonos-flatpak" /scratch-gpgme/matonos-flatpak.unstripped
  nm -a --defined-only /scratch-gpgme/matonos-flatpak.unstripped > /scratch-gpgme/matonos-flatpak.nm.txt
  strings /scratch-gpgme/matonos-flatpak.unstripped > /scratch-gpgme/matonos-flatpak.strings.txt
  if nm "$OUT/matonos-flatpak" 2>/dev/null | grep -E '(^|[[:space:]])(_gpgme_|assuan_)'; then
    echo 'ERROR: real GPGME/libassuan symbols remain in the binary' >&2
    exit 1
  fi
  if strings "$OUT/matonos-flatpak" | grep -E '(^|/)(usr/)?bin/gpg(2)?([[:space:]]|$)|libassuan|assuan_'; then
    echo 'ERROR: libassuan or a gpg exec path remains in the binary' >&2
    exit 1
  fi
fi
strip --strip-all "$OUT/matonos-flatpak"
ln -sfn matonos-flatpak "$OUT/flatpak"
ln -sfn matonos-flatpak "$OUT/ostree"
ln -sfn matonos-flatpak "$OUT/bwrap"
if [ "$GPGME_LITE" != 1 ]; then strip --strip-all "$OUT/gpg"; fi
if nm "$OUT/flatpak" 2>/dev/null | grep -E 'gdk_pixbuf_|as_metadata_'; then
  echo 'ERROR: Flatpak contains unwanted GdkPixbuf/AppStream symbols' >&2
  exit 1
fi
if strings "$OUT/matonos-flatpak" | grep -E 'gdk_pixbuf_|as_metadata_'; then
  echo 'ERROR: Flatpak contains unwanted GdkPixbuf/AppStream strings' >&2
  exit 1
fi
if [ "$GPGME_LITE" = 1 ]; then
  test ! -e "$OUT/gpg" || { echo 'ERROR: gpg must not be packaged with GPGME_LITE=1' >&2; exit 1; }
  bin_list='matonos-flatpak flatpak ostree bwrap'
else
  bin_list='matonos-flatpak flatpak ostree bwrap gpg'
fi
for bin in $bin_list; do
  test -x "$OUT/$bin"
  file "$OUT/$bin"
  readelf -l "$OUT/$bin" | grep -q INTERP && { echo "ERROR: $bin has INTERP" >&2; exit 1; } || :
  readelf -d "$OUT/$bin" 2>/dev/null | grep -q NEEDED && { echo "ERROR: $bin has NEEDED" >&2; exit 1; } || :
  printf '%s %s bytes\n' "$bin" "$(stat -c %s "$OUT/$bin")"
done
"$OUT/flatpak" --version
"$OUT/flatpak" remote-ls --help >/dev/null
"$OUT/ostree" --version
"$OUT/bwrap" --version
if "$OUT/matonos-flatpak" unknown-tool >/dev/null 2>&1; then
  echo 'ERROR: unknown multicall tool was accepted' >&2
  exit 1
else
  status=$?
  [ "$status" -eq 2 ] || { echo "ERROR: unknown tool returned $status, expected 2" >&2; exit 1; }
fi

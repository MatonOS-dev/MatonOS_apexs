#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE=${CACHE_DIR:-$HERE/cache}
SRC=$CACHE/src
BUILD=${BUILD_DIR:-$HERE/build}
PREFIX=${PREFIX:-$BUILD/prefix}
OUT=${OUT_DIR:-$HERE/out}
JOBS=${JOBS:-8}
[ "$JOBS" -le 8 ] || { echo 'JOBS must be <= 8' >&2; exit 2; }
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}
TOOL=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC=$TOOL/x86_64-linux-android36-clang
CXX=$TOOL/x86_64-linux-android36-clang++
AR=$TOOL/llvm-ar
STRIP=$TOOL/llvm-strip
READELF=$TOOL/llvm-readelf
HOST=x86_64-linux-android
MESON=${MESON:-$SRC/meson/meson.py}
CMAKE=${CMAKE:-/mnt/data/aosp/prebuilts/cmake/linux-x86/bin/cmake}
NINJA=${NINJA:-/mnt/data/aosp/prebuilts/build-tools/linux-x86/bin/ninja}
[ -x "$CMAKE" ] || CMAKE=$(command -v cmake || true)
[ -x "$NINJA" ] || NINJA=$(command -v ninja || true)
for exe in "$CC" "$CXX" "$AR" "$STRIP" "$READELF" "$CMAKE" "$NINJA"; do
  [ -x "$exe" ] || { echo "Missing build tool: $exe" >&2; exit 1; }
done
[ -f "$MESON" ] || { echo "Missing Meson module: $MESON" >&2; exit 1; }
mkdir -p "$BUILD" "$PREFIX" "$OUT"
export PATH="$(dirname "$NINJA"):$PATH"
export CC CXX AR RANLIB=$TOOL/llvm-ranlib STRIP
export CFLAGS='-O2 -fPIC'
export CXXFLAGS='-O2 -fPIC -stdlib=libc++'
COMPAT_CFLAGS="$CFLAGS -include $SRC/bionic-fill.h"
COMPAT_CXXFLAGS="$CXXFLAGS -include $SRC/bionic-fill.h"
export CPPFLAGS="-I$SRC/e2fsprogs-headers/usr/include"
export LDFLAGS="-L$PREFIX/lib -Wl,-Bstatic -lc++_static -lc++abi -Wl,-Bdynamic"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH=
PKG_CONFIG_STATIC="$BUILD/pkg-config-static"
cat > "$PKG_CONFIG_STATIC" <<'EOF'
#!/bin/sh
exec /usr/bin/pkg-config --static "$@"
EOF
chmod +x "$PKG_CONFIG_STATIC"
export PKG_CONFIG="$PKG_CONFIG_STATIC"
export PATH="$PREFIX/bin:$PATH"
export PATH="$SRC/gperf-host-tool/usr/bin:$PATH"
cat > "$BUILD/android.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$PKG_CONFIG_STATIC'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF
mkdir -p "$PREFIX/lib/pkgconfig"
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
Name: zlib
Description: Android NDK zlib
Version: 1.3.1
Libs: -lz
Cflags: -I\${includedir}
EOF
cat > "$PREFIX/lib/pkgconfig/e2p.pc" <<EOF
prefix=$PREFIX
includedir=$SRC/e2fsprogs-headers/usr/include
Name: e2p
Description: e2fsprogs header declarations used by OSTree
Version: 1.47.0
Libs:
Cflags: -I\${includedir}
EOF
meson() { python3 "$MESON" "$@"; }
MCOMMON="--cross-file $BUILD/android.ini --prefix $PREFIX --libdir lib --default-library static --prefer-static"

START_AT=${START_AT:-all}
if [ "$START_AT" = all ]; then
mkdir -p "$BUILD/libffi"; cd "$BUILD/libffi"
"$SRC/libffi/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static CFLAGS="$CFLAGS"
make -j"$JOBS"; make install
"$CMAKE" -S "$SRC/pcre2" -B "$BUILD/pcre2" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF
"$CMAKE" --build "$BUILD/pcre2" --target install --parallel "$JOBS"
meson setup "$BUILD/glib" "$SRC/glib" $MCOMMON -Dtests=false -Dman-pages=disabled -Dintrospection=disabled -Ddocumentation=false -Dselinux=disabled
"$NINJA" -C "$BUILD/glib" -j "$JOBS" install
"$CMAKE" -S "$SRC/zstd/build/cmake" -B "$BUILD/zstd" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF
"$CMAKE" --build "$BUILD/zstd" --target install --parallel "$JOBS"
meson setup "$BUILD/libxml2" "$SRC/libxml2" $MCOMMON -Ddocs=disabled -Dpython=disabled -Dminimum=false -Doutput=enabled -Dzlib=enabled
"$NINJA" -C "$BUILD/libxml2" -j "$JOBS" install
meson setup "$BUILD/json-glib" "$SRC/json-glib" $MCOMMON -Dtests=false -Dman=false -Dintrospection=disabled
"$NINJA" -C "$BUILD/json-glib" -j "$JOBS" install
meson setup "$BUILD/libxmlb" "$SRC/libxmlb" $MCOMMON -Dtests=false -Dcli=false -Dintrospection=false -Dgtkdoc=false
"$NINJA" -C "$BUILD/libxmlb" -j "$JOBS" install
"$CMAKE" -S "$SRC/boringssl" -B "$BUILD/boringssl" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
"$CMAKE" --build "$BUILD/boringssl" --parallel "$JOBS" --target crypto ssl
mkdir -p "$PREFIX/lib" "$PREFIX/include/openssl"
cp "$BUILD/boringssl/libcrypto.a" "$PREFIX/lib/"; cp "$BUILD/boringssl/libssl.a" "$PREFIX/lib/"
cp -R "$SRC/boringssl/include/openssl/." "$PREFIX/include/openssl/"
mkdir -p "$BUILD/boringssl-headers/src"
ln -sfn "$SRC/boringssl/include" "$BUILD/boringssl-headers/src/include"
if [ ! -d "$BUILD/gpgme-lite-src" ]; then cp -R "$SRC/DullPGP" "$BUILD/gpgme-lite-src"; fi
(cd "$BUILD/gpgme-lite-src" && make clean && make -j"$JOBS" CC="$CC" AR="$AR" CFLAGS="$CFLAGS" BSSL_SRC="$BUILD/boringssl-headers" BSSL_LIBDIR="$PREFIX/lib" PREFIX="$PREFIX" LIBDIR="$PREFIX/lib" INCLUDEDIR="$PREFIX/include/gpgme-lite")
install -D -m644 "$BUILD/gpgme-lite-src/libgpgme-lite.a" "$PREFIX/lib/libgpgme-lite.a"
install -d "$PREFIX/include/gpgme-lite"; cp "$SRC/DullPGP/include/"*.h "$PREFIX/include/gpgme-lite/"
cat > "$PREFIX/lib/pkgconfig/gpgme.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include/gpgme-lite
Name: gpgme-lite
Description: DullPGP gpgme-lite
Version: 2.0.1
Libs: -L\${libdir} -lgpgme-lite
Libs.private: -L\${libdir} -lcrypto
Cflags: -I\${includedir}
EOF
cat > "$PREFIX/lib/pkgconfig/gpg-error.pc" <<EOF
prefix=$PREFIX
includedir=\${prefix}/include/gpgme-lite
Name: gpg-error
Description: gpgme-lite error compatibility
Version: 1.0
Libs:
Cflags: -I\${includedir}
EOF

mkdir -p "$BUILD/libseccomp"; cd "$BUILD/libseccomp"
"$SRC/libseccomp/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static --disable-python --disable-tests CFLAGS="$CFLAGS"
make -j"$JOBS"; make install
if [ ! -d "$BUILD/libcap-src" ]; then cp -R "$SRC/libcap" "$BUILD/libcap-src"; fi
make -C "$BUILD/libcap-src/libcap" -j"$JOBS" CC="$CC" AR="$AR" RANLIB="$RANLIB" prefix="$PREFIX" lib=lib LIBDIR="$PREFIX/lib" SHARED=no PTHREADS=no BUILD_CC=cc
make -C "$BUILD/libcap-src/libcap" install-static CC="$CC" AR="$AR" RANLIB="$RANLIB" prefix="$PREFIX" lib=lib LIBDIR="$PREFIX/lib" SHARED=no PTHREADS=no BUILD_CC=cc
meson setup "$BUILD/bubblewrap" "$SRC/bubblewrap" $MCOMMON -Dtests=false -Dman=disabled -Dc_args="$COMPAT_CFLAGS"
"$NINJA" -C "$BUILD/bubblewrap" -j "$JOBS"

mkdir -p "$BUILD/libarchive"; cd "$BUILD/libarchive"
"$SRC/libarchive/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static --without-openssl --without-nettle --without-mbedtls --without-lzo2 --without-bz2lib --without-lz4 --without-zstd --without-xml2 CPPFLAGS="-I$SRC/libarchive/contrib/android/include $CPPFLAGS" CFLAGS="$CFLAGS"
make -j"$JOBS"; make install
mkdir -p "$BUILD/curl"; cd "$BUILD/curl"
"$SRC/curl/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static --with-ssl="$PREFIX" --with-zlib="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot" --with-ca-path=/apex/com.android.conscrypt/cacerts --without-ca-bundle --without-ca-fallback --disable-ftp --enable-file --disable-websockets --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --without-libssh2 --without-nghttp2 --without-libidn2 --without-libpsl --without-brotli CFLAGS="$CFLAGS" CPPFLAGS="$CPPFLAGS" LDFLAGS="$LDFLAGS -lstdc++ -pthread" LIBS='-lstdc++ -pthread'
make -j"$JOBS"; make install
# Configure probes need -lstdc++ with BoringSSL, but Android resolves that
# compatibility name to libc++_shared. The final CLIs explicitly link NDK
# libc++.a, so drop the probe-only token from libtool dependency metadata.
sed -i '/^dependency_libs=/ s/[[:space:]]-lstdc++//g' "$PREFIX/lib/libcurl.la"
"$CMAKE" -S "$SRC/liblzma" -B "$BUILD/liblzma" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_DOC=OFF -DXZ_NLS=OFF
"$CMAKE" --build "$BUILD/liblzma" --target install --parallel "$JOBS"
fi

mkdir -p "$BUILD/ostree"; cd "$BUILD/ostree"
if [ ! -x "$SRC/ostree/configure" ]; then
  REPO=$(CDPATH= cd -- "$HERE/.." && pwd)
  ALPINE_SCRATCH=${ALPINE_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
  ALPINE_WORK=$(CDPATH= cd -- "$(dirname "$CACHE")" && pwd)
  [ -d "$ALPINE_SCRATCH/rootfs/x86_64" ] || { echo "Alpine rootfs missing: $ALPINE_SCRATCH/rootfs/x86_64" >&2; exit 1; }
  [ -d "$ALPINE_SCRATCH/inputs" ] || { echo "Alpine inputs missing: $ALPINE_SCRATCH/inputs" >&2; exit 1; }
  [ -e "$ALPINE_WORK/rootfs" ] || ln -s "$ALPINE_SCRATCH/rootfs" "$ALPINE_WORK/rootfs"
  [ -e "$ALPINE_WORK/inputs" ] || ln -s "$ALPINE_SCRATCH/inputs" "$ALPINE_WORK/inputs"
  SCRATCH="$ALPINE_WORK" ARCH=x86_64 ALPINE_INPUTS_DIR="$ALPINE_WORK/inputs" GPGME_LITE_SRC="$SRC/DullPGP" \
    "$REPO/flatpak/alpine-enter.sh" sh -c 'cd /scratch/cache/src/ostree && ACLOCAL_PATH=/scratch/cache/src/DullPGP NOCONFIGURE=1 ./autogen.sh'
fi
ac_cv_c_undeclared_builtin_options='none needed' "$SRC/ostree/configure" --host="$HOST" --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-shared --enable-static --with-curl --with-gpgme --without-soup --without-soup3 --without-selinux --without-libmount --without-avahi --disable-rofiles-fuse --disable-man --disable-installed-tests --without-composefs --with-crypto=glib CC="$CC" CFLAGS="$COMPAT_CFLAGS -I$PREFIX/include/gpgme-lite" LIBS="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/36/libc++.a" PKG_CONFIG="$PKG_CONFIG" PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR"
make -j"$JOBS" ostree
make install

FLATPAK_CFLAGS="-O2 -fPIC -include $SRC/bionic-fill.h"
FLATPAK_CXXFLAGS="-O2 -fPIC -stdlib=libc++ -include $SRC/bionic-fill.h"
meson setup "$BUILD/flatpak" "$SRC/flatpak" $MCOMMON -Dtests=false -Dinstalled_tests=false -Ddocbook_docs=disabled -Dgtkdoc=disabled -Dman=disabled -Dgir=disabled -Ddconf=disabled -Dsystemd=disabled -Dsystem_helper=disabled -Dmalcontent=disabled -Dselinux_module=disabled -Dauto_sideloading=false -Dxauth=disabled -Dsystem_bubblewrap=/apex/com.matonos.flatpak/bin/matonos-bwrap -Dsystem_dbus_proxy=/apex/com.matonos.flatpak/bin/xdg-dbus-proxy -Dc_args="$FLATPAK_CFLAGS" -Dcpp_args="$FLATPAK_CXXFLAGS" -Dc_link_args="$LDFLAGS" -Dcpp_link_args="$LDFLAGS"
"$NINJA" -C "$BUILD/flatpak" -j "$JOBS"
install -D -m755 "$BUILD/flatpak/app/flatpak" "$OUT/flatpak.unstripped"
install -D -m755 "$BUILD/ostree/ostree" "$OUT/ostree.unstripped"
install -D -m755 "$BUILD/bubblewrap/bwrap" "$OUT/bwrap.unstripped"
for bin in flatpak ostree bwrap; do "$STRIP" --strip-all "$OUT/$bin.unstripped" -o "$OUT/$bin"; done
python3 "$HERE/license-gate.py"
{
  echo 'flatpak-bionic source manifest'; cat "$HERE/sources.lock"; echo
  for bin in flatpak ostree bwrap; do sha256sum "$OUT/$bin"; done
} > "$OUT/SOURCE"
echo "Artifacts and provenance written to $OUT"

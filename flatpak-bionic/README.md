# Android bionic Flatpak recipes

This directory builds the bionic Flatpak toolchain for `x86_64-linux-android36` using NDK r30. It pins all source repositories and release archives in `sources.lock` and installs cross-built libraries under `build/prefix`.

Run from the repository root:

```sh
flatpak-bionic/fetch.sh
JOBS=8 flatpak-bionic/build.sh
```

`CACHE_DIR`, `BUILD_DIR`, `PREFIX`, `OUT_DIR`, `ANDROID_NDK`, `MESON`, `CMAKE`, and `NINJA` can override the defaults. The Meson 1.12.1 source is locked and used from the fetched cache. CMake and Ninja use the AOSP prebuilts when available and otherwise resolve from `PATH`. `fetch.sh` verifies release archive SHA-256 digests and checks every Git checkout against its exact commit before exporting the sources.

The Git inputs are GLib, OSTree, OSTree's libglnx and bsdiff subprojects, the MatonOS Flatpak fork, Flatpak's libglnx and variant-schema-compiler subprojects, bionic-fill, and DullPGP. Flatpak is cloned from the local `~/matonos/repos/flatpak` checkout at `c2fc8fcc`; bionic-fill and DullPGP are cloned from their local MatonOS checkouts at the revisions in the lock. `gpgme.m4` is kept beside this README as an explicit recipe input until DullPGP absorbs it. Release archives cover GLib's static dependencies, compression, XML, JSON, seccomp/capability, TLS/HTTP, archive and bubblewrap libraries. The e2fsprogs development package contributes the OSTree `ext2fs` headers; a pinned native gperf package supplies libseccomp's generated syscall tables; Android zlib comes from the pinned NDK sysroot.

Build order follows dependency edges: libffi/PCRE2 and GLib; zstd/XML/JSON libraries; BoringSSL and DullPGP's gpgme-lite; seccomp, libcap and bubblewrap; libarchive, curl and liblzma; then static OSTree and Flatpak. All output binaries are stripped to `out/{flatpak,ostree,bwrap}`, with their unstripped build inputs retained and an `out/SOURCE` manifest containing their hashes and source pins.

The curl build uses `/apex/com.android.conscrypt/cacerts` as its CA directory and disables the CA bundle and fallback. OSTree links statically, and the final Flatpak and OSTree links use NDK static libc++. bionic-fill is force included in each project that needs the compatibility declarations, including Flatpak's bundled libglnx; Flatpak source files remain unchanged. The Flatpak fork already carries the required AppStream, revokefs/FUSE and gdk-pixbuf/icon-validator removals.

## Source changes and licence policy

The no-patch rule is that recipe builds do not edit upstream source. Only the pinned MatonOS Flatpak fork contains project changes; bionic-fill supplies bionic API gaps through a force-included header. OSTree's generated `configure` is produced with `NOCONFIGURE=1 ./autogen.sh` in the existing Alpine build environment, then used for the NDK cross configure. All other inputs remain their pinned upstream trees and tarballs.

`license-gate.py` checks the declared licence expressions in `licenses.tsv` and fails on GPLv3 or LGPLv3 identifiers. Run it directly with Python 3 to check the manifest.

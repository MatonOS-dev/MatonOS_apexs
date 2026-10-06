# Android bionic Flatpak recipes

This directory builds the bionic Flatpak toolchain for `x86_64-linux-android36` using NDK r30. It pins all source repositories and release archives in `sources.lock` and installs cross-built libraries under `build/prefix`.

Run from the repository root:

```sh
flatpak/fetch.sh
JOBS=8 flatpak/build.sh
```

`CACHE_DIR`, `BUILD_DIR`, `PREFIX`, `OUT_DIR`, `ANDROID_NDK`, `MESON`, `CMAKE`, and `NINJA` can override the defaults. The Meson 1.12.1 source is locked and used from the fetched cache. CMake and Ninja use the AOSP prebuilts when available and otherwise resolve from `PATH`. `fetch.sh` verifies release archive SHA-256 digests and checks every Git checkout against its exact commit before exporting the sources.

The Git inputs are GLib, OSTree, OSTree's libglnx and bsdiff subprojects, the MatonOS Flatpak fork, Flatpak's libglnx and variant-schema-compiler subprojects, bionic-fill, and DullPGP. All Git inputs are fetched from their public pinned URLs. Flatpak is fetched from `https://github.com/MatonOS-dev/flatpak` at `c2fc8fcc`; bionic-fill and DullPGP are also fetched from MatonOS-dev. `gpgme.m4` is kept beside this README as an explicit recipe input until DullPGP absorbs it. Release archives cover GLib's static dependencies, compression, XML, JSON, seccomp/capability, TLS/HTTP, archive and bubblewrap libraries. The e2fsprogs development package contributes the OSTree `ext2fs` headers; a pinned native gperf package supplies libseccomp's generated syscall tables; Android zlib comes from the pinned NDK sysroot.

Build order follows dependency edges: libffi/PCRE2 and GLib; zstd/XML/JSON libraries; BoringSSL and DullPGP's gpgme-lite; seccomp, libcap and bubblewrap; libarchive, curl and liblzma; then static OSTree and Flatpak. The top-level `build.sh` flow emits all five stripped APEX binaries to `out/`: the Flatpak/OSTree/bubblewrap multicall ELF `matonos-flatpak`, plus `flatpak-env-wrapper`, `matonos-bwrap`, `matonos-app-exec`, and `matonos-flatpak-store`. `out/SOURCE` records source pins and SHA-256 hashes. `matonos-app-exec` is fully static for app sandboxes.

The curl build uses `/apex/com.android.conscrypt/cacerts` as its CA directory and disables the CA bundle and fallback. OSTree links statically, and the final Flatpak and OSTree links use NDK static libc++. bionic-fill is force included in each project that needs the compatibility declarations, including Flatpak's bundled libglnx; Flatpak source files remain unchanged. The Flatpak fork already carries the required AppStream, revokefs/FUSE and gdk-pixbuf/icon-validator removals.

## Source changes and licence policy

The no-patch rule is that recipe builds do not edit upstream source. Only the pinned MatonOS Flatpak fork contains project changes; bionic-fill supplies bionic API gaps through a force-included header. OSTree's generated `configure` is produced with `NOCONFIGURE=1 ./autogen.sh` in the existing Alpine build environment, then used for the NDK cross configure. All other inputs remain their pinned upstream trees and tarballs.

`license-gate.py` checks the declared licence expressions in `licenses.tsv` and fails on GPLv3 or LGPLv3 identifiers. Run it directly with Python 3 to check the manifest.

`make-multicall.py` links the three project object sets into the single CLI ELF. It renames each project's `main` symbol in temporary object copies, leaving fetched source and ordinary build objects untouched. The OSTree archive is repacked after installation to retain relocatable object members only.

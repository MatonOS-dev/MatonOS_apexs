# MatonOS APEXes

Build files for MatonOS's package-format APEXes. One directory per format.

- `flatpak/` — `com.matonos.flatpak`: the Flatpak stack built for Android's
  bionic libc with the NDK (x86_64, API 36). Upstream glib, OSTree and
  bubblewrap are built unmodified; bionic's missing glibc-isms come from
  [bionic-fill](https://github.com/MatonOS-dev/bionic-fill), force-included
  from the build flags. Flatpak itself is the
  [MatonOS-dev/flatpak](https://github.com/MatonOS-dev/flatpak) fork
  (`matonos/v26.10`: AppStream, FUSE/revokefs and gdk-pixbuf removed).
  OpenPGP verification uses DullPGP (MatonOS-dev/DullPGP) on BoringSSL;
  curl uses Android's Conscrypt CA store. Binaries link only stable system
  libraries (`libc`, `libm`, `libdl`, `libz`); everything else is static.
  Signing happens in the MatonOS device tree; no private keys live here.

Build: `flatpak/fetch.sh && JOBS=8 flatpak/build.sh` — see
[flatpak/README.md](flatpak/README.md). Every input is pinned in
`flatpak/sources.lock` (commits or SHA-256).

Status: cross-builds reproducibly; on an Android VM the bionic `flatpak`
adds Flathub and reads remote metadata as an app UID (DNS through netd,
TLS via Conscrypt, signatures via DullPGP). Not yet verified: object
downloads from Flathub (the musl build needed a curl user agent because
Flathub returned 403 for `libostree/…`), and the single multicall binary.
The previous Alpine/musl build was retired on 2026-10-06 (see git history).

The repository's build tooling is Apache-2.0 (`LICENSE`). The statically
linked APEX components retain their own terms; see
[LICENSES.md](flatpak/LICENSES.md) and the component inventory in
`flatpak/licenses.tsv`. The licence gate fails the build on (L)GPLv3.

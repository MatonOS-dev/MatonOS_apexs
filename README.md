# MatonOS APEXes

Build files for MatonOS's package-format APEXes. One directory per format.

- `flatpak/` — `com.matonos.flatpak`: a fully static (musl) Flatpak stack:
  one multicall binary `matonos-flatpak` (flatpak + ostree + bwrap), built
  from Alpine recipes and the MatonOS-dev/flatpak fork, verified with
  DullPGP (MatonOS-dev/DullPGP) on BoringSSL. Signing happens in the
  MatonOS device tree; no private keys live here.

Status: builds and passes the host layout test (Alpine 3.24 stable:
flatpak 1.16.6, ostree 2025.7, bubblewrap 0.12.0; DullPGP; curl on BoringSSL
with FILE/HTTP/HTTPS only; no OpenSSL, no GnuPG). `matonos-flatpak` is
~10.6 MB. Alpine binaries/sources: see `flatpak/alpine-assets.lock`.

Licence: Apache-2.0 (`LICENSE`). Third-party sources built here keep their own licences.

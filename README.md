# MatonOS APEXes

Build files for MatonOS's package-format APEXes. One directory per format.

- `flatpak/` — `com.matonos.flatpak`: a fully static (musl) Flatpak stack:
  one multicall binary `matonos-flatpak` (flatpak + ostree + bwrap), built
  from Alpine recipes and the MatonOS-dev/flatpak fork, verified with
  DullPGP (MatonOS-dev/DullPGP) on BoringSSL. Signing happens in the
  MatonOS device tree; no private keys live here.

Status: work in progress (moving from Alpine 3.24 to Alpine edge).

Licence: Apache-2.0 (`LICENSE`). Third-party sources built here keep their own licences.

# MatonOS Flatpak APEX licenses

This bundle statically links the components listed in `licenses/linked-components.json`.
The selected license option for every SPDX alternative is recorded there. Components with GPL-3.0 or LGPL-3.0 as the only usable option are rejected by the build gate.

For LGPL-2.1 components, MatonOS provides the corresponding source through the pinned inputs in `flatpak/sources.lock` (upstream commits and release archives, and the listed forks). Users can relink the binaries by running `flatpak/fetch.sh` and `flatpak/build.sh`; the build regenerates the binaries and the `SOURCE` manifest.

See `licenses/NOTICE` and the per-component license texts in `licenses/`.

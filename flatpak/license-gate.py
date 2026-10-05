#!/usr/bin/env python3
"""Generate per-ELF component SPDX records from GNU ld maps and Alpine recipes."""
import argparse
import json
import re
import shutil
import sys
import tarfile
from pathlib import Path

ARCHIVES = {
    "libarchive.a": "libarchive", "libblkid.a": "util-linux-libs",
    "libbz2.a": "bzip2", "libc.a": "musl", "libcap.a": "libcap",
    "libcrypto.a": "boringssl", "libcurl.a": "curl",
    "libeconf.a": "libeconf", "libffi.a": "libffi",
    "libgcc.a": "gcc-libgcc", "libgio-2.0.a": "glib", "libglib-2.0.a": "glib",
    "libgmodule-2.0.a": "glib", "libgobject-2.0.a": "glib",
    "libgpgme-lite.a": "DullPGP", "libintl.a": "gettext-libintl",
    "libjson-glib-1.0.a": "json-glib", "liblz4.a": "lz4",
    "liblzma.a": "xz", "libmount.a": "util-linux-libs",
    "libpcre2-8.a": "pcre2", "libseccomp.a": "libseccomp",
    "libssl.a": "boringssl", "libstdc++.a": "gcc-libstdc++",
    "libxml2.a": "libxml2", "libz.a": "zlib", "libzstd.a": "zstd",
    "libostree-1.a": "ostree", "libotutil.a": "ostree",
    "libotcore.a": "ostree", "libglnx.a": "flatpak",
    "libxmlb.a": "libxmlb",
}
CUSTOM = {
    "flatpak": ("Flatpak", "1.16.6", "GPL-2.0-or-later"),
    "ostree": ("OSTree", "2025.7", "LGPL-2.0-or-later"),
    "bubblewrap": ("bubblewrap", "0.12.0", "LGPL-2.1-or-later"),
    "curl": ("curl", "8.22.0", "curl"),
    "json-glib": ("JSON-GLib", "1.10.8", "LGPL-2.1-or-later"),
    "libxmlb": ("libxmlb", "0.3.27", "LGPL-2.1-or-later"),
    "boringssl": ("BoringSSL", "0cd1f6a94b670e6b61af95a06e71e690ae8e3262", "Apache-2.0"),
    "DullPGP": ("DullPGP gpgme-lite", "2.0.1", "LGPL-2.1-or-later"),
    # Alpine's util-linux APKBUILD aggregates licenses for many binaries.
    # These linked libraries are the upstream LGPL-2.1-or-later components.
    "util-linux-libs": ("util-linux libblkid/libmount", "2.42.3", "LGPL-2.1-or-later"),
    # The APKBUILD covers gettext's GPL tools as well as libintl. The linked
    # libintl runtime is specifically LGPL-2.1-or-later upstream.
    "gettext-libintl": ("GNU gettext libintl", "1.0", "LGPL-2.1-or-later"),
    "gcc-libgcc": ("GCC libgcc runtime", "15.2.0", "GPL-3.0-or-later WITH GCC-exception-3.1"),
    "gcc-libstdc++": ("GCC libstdc++ runtime", "15.2.0", "GPL-3.0-or-later WITH GCC-exception-3.1"),
}
V3 = {"GPL-3.0-only", "GPL-3.0-or-later", "LGPL-3.0-only", "LGPL-3.0-or-later"}


def recipe_license(path: Path) -> str:
    if not path.is_file():
        raise ValueError(f"missing APKBUILD for linked component {path.parent.name}")
    text = path.read_text(errors="replace")
    match = re.search(r'^license\s*=\s*["\']([^"\']+)["\']', text, re.M)
    if not match:
        raise ValueError(f"cannot read APKBUILD license= from {path}")
    return match.group(1).strip()


def tokens(expr: str):
    return re.findall(r"\(|\)|\bAND\b|\bOR\b|[A-Za-z0-9.+-]+", expr)


def parse_expr(expr: str):
    ts = tokens(expr)
    pos = 0

    def atom():
        nonlocal pos
        if pos >= len(ts):
            raise ValueError(f"invalid SPDX expression: {expr}")
        if ts[pos] == "(":
            pos += 1
            node = disjunction()
            if pos >= len(ts) or ts[pos] != ")":
                raise ValueError(f"unbalanced SPDX expression: {expr}")
            pos += 1
            return node
        value = ts[pos]
        pos += 1
        if pos < len(ts) and ts[pos] == "WITH":
            pos += 1
            if pos >= len(ts):
                raise ValueError(f"missing SPDX exception in: {expr}")
            exception = ts[pos]
            pos += 1
            return ("with", ("id", value), exception)
        return ("id", value)

    def conjunction():
        nonlocal pos
        nodes = [atom()]
        while pos < len(ts) and ts[pos] == "AND":
            pos += 1
            nodes.append(atom())
        return nodes[0] if len(nodes) == 1 else ("and", nodes)

    def disjunction():
        nonlocal pos
        nodes = [conjunction()]
        while pos < len(ts) and ts[pos] == "OR":
            pos += 1
            nodes.append(conjunction())
        return nodes[0] if len(nodes) == 1 else ("or", nodes)

    node = disjunction()
    if pos != len(ts):
        raise ValueError(f"unparsed SPDX expression: {expr}")
    return node


def choose(node):
    kind = node[0]
    if kind == "id":
        ident = node[1]
        return (None, [ident]) if ident in V3 else (ident, [ident])
    if kind == "with":
        base, exception = node[1], node[2]
        if base[1] in V3 and exception == "GCC-exception-3.1":
            return (f"{base[1]} WITH {exception}", [base[1], exception])
        return choose(base)
    if kind == "and":
        chosen, leaves = [], []
        for child in node[1]:
            c, ls = choose(child)
            if c is None:
                return None, leaves + ls
            chosen.append(c)
            leaves.extend(ls)
        return " AND ".join(chosen), leaves
    options = [choose(child) for child in node[1]]
    # A GPL-2.0-or-later alternative is not accepted as a rescue for a v3
    # reciprocal term: retaining the explicit v2+ option would leave a path
    # to GPLv3 for this statically linked LGPL-2.1 bundle.
    v3_reciprocal = any(x in {"LGPL-3.0-only", "LGPL-3.0-or-later"} for _, ls in options for x in ls)
    valid = [(c, ls) for c, ls in options if c is not None and not
             (v3_reciprocal and c == "GPL-2.0-or-later")]
    if not valid:
        return None, [x for _, ls in options for x in ls]
    return valid[0]


def license_docs(component: str, build: Path, alpine_src: Path, dullpgp: Path):
    source_component = {"util-linux-libs": "util-linux", "gettext-libintl": "gettext",
                        "gcc-libgcc": "gcc", "gcc-libstdc++": "gcc"}.get(component, component)
    roots = [build / source_component, alpine_src / source_component]
    if component == "boringssl": roots.append(build / "boringssl-src")
    if component == "DullPGP": roots.append(dullpgp)
    names = ("COPYING", "COPYING.LIB", "COPYING.RUNTIME", "COPYING3", "COPYING3.LIB",
             "LICENSE", "License", "license", "LICENSE.txt", "LICENSE.md",
             "COPYRIGHT", "Copyright")
    found = {}
    for root in roots:
        if not root.exists():
            continue
        for name in names:
            hits = list(root.rglob(name)) if root.is_dir() else []
            for hit in hits:
                if hit.is_file() and hit.stat().st_size > 100:
                    try: data = hit.read_bytes()
                    except OSError: continue
                    found.setdefault(name, data)
        # Alpine source bundles retain signed source archives instead of
        # checked-out build trees. Read their embedded upstream license files.
        for archive in root.rglob("*.tar*") if root.is_dir() else []:
            try:
                with tarfile.open(archive, "r:*") as tar:
                    for member in tar.getmembers():
                        base = Path(member.name).name
                        if base in names and member.isfile() and member.size > 100 and base not in found:
                            stream = tar.extractfile(member)
                            if stream: found[base] = stream.read()
            except (OSError, tarfile.TarError):
                continue
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--alpine-src", type=Path, required=True)
    ap.add_argument("--apk-lock", type=Path, required=True)
    ap.add_argument("--dullpgp", type=Path, required=True)
    ap.add_argument("--repo-flatpak", type=Path, required=True)
    args = ap.parse_args()
    versions = {}
    for line in args.apk_lock.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2:
            versions[fields[0]] = fields[1]
    linked_by_binary = {}
    for binary in ("matonos-flatpak", "matonos-bwrap", "matonos-app-exec", "flatpak-env-wrapper", "matonos-flatpak-store"):
        mapfile = args.output / f"{binary}.map"
        if not mapfile.is_file():
            raise ValueError(f"link map missing for staged binary: {mapfile}")
        text = mapfile.read_text(errors="replace")
        components = set()
        for archive in re.findall(r"([^\s()]+\.a)\(", text):
            mapped = ARCHIVES.get(Path(archive).name)
            if not mapped:
                raise ValueError(f"unmapped linked archive in {binary}: {archive}")
            components.add(mapped)
        if binary == "matonos-flatpak":
            # These sources were folded into relocatable objects before the
            # final link and therefore do not appear as archive paths.
            components.update(("flatpak", "ostree", "bubblewrap"))
        linked_by_binary[binary] = components

    all_components = sorted(set().union(*linked_by_binary.values()))
    records = {}
    docs = {}
    version_packages = {"flatpak": "flatpak", "ostree": "ostree", "bubblewrap": "bubblewrap",
                        "curl": "libcurl", "json-glib": "json-glib", "libxmlb": "libxmlb",
                        "util-linux-libs": "util-linux-dev", "gettext-libintl": "gettext-libs",
                        "gcc-libgcc": "libgcc-static", "gcc-libstdc++": "libstdc++-dev"}
    for component in all_components:
        source_component = {"util-linux-libs": "util-linux", "gettext-libintl": "gettext",
                            "gcc-libgcc": "gcc", "gcc-libstdc++": "gcc"}.get(component, component)
        if component in CUSTOM:
            name, version, expr = CUSTOM[component]
            package = version_packages.get(component)
            if package in versions:
                version = versions[package]
            recipe = args.alpine_src / component / "APKBUILD"
            if component in {"util-linux-libs", "gettext-libintl", "gcc-libgcc", "gcc-libstdc++"}:
                recipe = args.alpine_src / source_component / "APKBUILD"
            if recipe.is_file():
                expr = recipe_license(recipe)
            if component in {"util-linux-libs", "gettext-libintl", "gcc-libgcc", "gcc-libstdc++"}:
                expr = CUSTOM[component][2]
        else:
            expr = recipe_license(args.alpine_src / component / "APKBUILD")
            package = {"util-linux": "util-linux-dev", "gcc": "gcc", "glib": "glib-dev", "musl": "musl-dev"}.get(component, component)
            version = versions.get(package)
            if not version:
                candidates = sorted((n, v) for n, v in versions.items() if n == component or n.startswith(component + "-"))
                if not candidates:
                    raise ValueError(f"no locked APK version for {component}")
                version = candidates[0][1]
            name = component
        # Alpine retains the historical non-SPDX token Public-Domain. Give it
        # an explicit SPDX LicenseRef while preserving the APKBUILD spelling.
        spdx_expr = expr.replace("Public-Domain", "LicenseRef-Public-Domain")
        chosen, terms = choose(parse_expr(spdx_expr))
        if chosen is None:
            raise ValueError(f"license gate rejected {name}: {expr} (no allowed choice)")
        records[component] = {"name": name, "version": version, "spdx_license": spdx_expr,
                              "upstream_license": expr,
                              "chosen_license": chosen, "license_terms": terms}
        docs_for_component = license_docs(component, args.build, args.alpine_src, args.dullpgp)
        if not docs_for_component:
            raise ValueError(f"license text unavailable for {name}; refusing to stage an incomplete notice set")
        docs[component] = docs_for_component

    out = args.repo_flatpak / "licenses"
    out.mkdir(parents=True, exist_ok=True)
    for child in out.iterdir():
        if child.is_file(): child.unlink()
        elif child.is_dir(): shutil.rmtree(child)
    manifest = {"schema_version": 1, "generated_from": "GNU ld link maps", "binaries": {}}
    for binary, components in sorted(linked_by_binary.items()):
        manifest["binaries"][binary] = [records[c] for c in sorted(components)]
    (out / "linked-components.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    notices = ["MatonOS Flatpak APEX component notices", "", "The following components are statically linked into the listed binaries.",
               "Each component's declared SPDX expression and selected license option are recorded in linked-components.json.", ""]
    for component, rec in sorted(records.items()):
        notices.append(f"- {rec['name']} {rec['version']} — {rec['chosen_license']} (SPDX: {rec['spdx_license']}; source declaration: {rec['upstream_license']})")
        with (out / f"{component}.txt").open("wb") as license_file:
            for title, contents in sorted(docs[component].items()):
                license_file.write((f"===== {title} =====\n\n").encode())
                license_file.write(contents.rstrip() + b"\n\n")
    (out / "NOTICE").write_text("\n".join(notices) + "\n")
    licenses_md = ["# MatonOS Flatpak APEX licenses", "", "This bundle statically links the components listed in `licenses/linked-components.json`.",
                   "The selected license option for every SPDX alternative is recorded there. Components with GPL-3.0 or LGPL-3.0 as the only usable option are rejected by the build gate.", "",
                   "For LGPL-2.1 components, MatonOS provides the corresponding source in this repository, the pinned Alpine release assets, and the listed forks. Users can relink the binaries by running `flatpak/build-static.sh` with the pinned Alpine inputs; the build regenerates the binaries and linked component list.", "",
                   "See `licenses/NOTICE` and the per-component license texts in `licenses/`.", ""]
    (args.repo_flatpak / "LICENSES.md").write_text("\n".join(licenses_md))
    print(f"License gate passed: {len(all_components)} components across {len(linked_by_binary)} binaries")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        print(f"license gate: {exc}", file=sys.stderr)
        sys.exit(1)

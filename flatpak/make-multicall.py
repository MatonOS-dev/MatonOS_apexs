#!/usr/bin/env python3
"""Relink the already-built CLI objects as the device's argv[0] multicall ELF."""
import glob
import os
import re
import shlex
import subprocess
import sys
import tempfile

build, output, objcopy, cc = sys.argv[1:]
flatpak = os.path.join(build, "flatpak")
ostree = os.path.join(build, "ostree")
bwrap = os.path.join(build, "bubblewrap")

def run(args, cwd=None, capture=False):
    return subprocess.check_output(args, cwd=cwd, text=True) if capture else subprocess.check_call(args, cwd=cwd)

# Rename each project's entry point without changing fetched source.
cmds = run(["ninja", "-C", flatpak, "-t", "commands", "app/flatpak"], capture=True)
link = shlex.split(cmds.strip().splitlines()[-1])
# Retain the full Flatpak link and its static dependency set; its dependency graph
# already includes libostree, GLib, curl, archive, TLS and libc++.
args = []
i = 1
while i < len(link):
    if link[i] == "-o":
        i += 2
        continue
    args.append(link[i])
    i += 1

# Add OSTree's CLI-only translation units, excluding its library (already linked
# by Flatpak), then bubblewrap's implementation units.
ostree_objects = run(
    ["make", "-s", "-f", "Makefile", "--eval=print-ostree:;@echo $(ostree_OBJECTS)", "print-ostree"],
    cwd=ostree, capture=True,
).split()
objects = [os.path.join(ostree, x) for x in ostree_objects]
objects.extend(sorted(glob.glob(os.path.join(bwrap, "bwrap.p", "*.c.o"))))

source = os.path.join(os.path.dirname(__file__), "multicall.c")
with tempfile.TemporaryDirectory(prefix="matonos-multicall-") as td:
    renamed = {
        os.path.join(flatpak, "app/flatpak.p/flatpak-main.c.o"): "flatpak_main",
        os.path.join(ostree, "src/ostree/ostree-main.o"): "ostree_cli_main",
        os.path.join(bwrap, "bwrap.p/bubblewrap.c.o"): "bwrap_main",
    }
    replacements = {}
    for index, (obj, symbol) in enumerate(renamed.items()):
        copied = os.path.join(td, "entry-%d.o" % index)
        run(["cp", obj, copied])
        run([objcopy, "--redefine-sym", "main=" + symbol, copied])
        replacements[obj] = copied
    args = [replacements.get(os.path.abspath(os.path.join(flatpak, x)) if not os.path.isabs(x) else x, x) for x in args]
    objects = [replacements.get(x, x) for x in objects]
    insertion = next((i for i, x in enumerate(args) if x.endswith(".a") or x == "-Wl,--start-group"), len(args))
    args[insertion:insertion] = objects
    group_end = next((i for i, x in enumerate(args) if x == "-Wl,--end-group"), len(args))
    args.insert(group_end, os.path.join(build, "prefix/lib/libcap.a"))
    dispatcher = os.path.join(td, "multicall.o")
    run([cc, "-O2", "-fPIC", "-c", source, "-o", dispatcher])
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    run([link[0], *args, dispatcher, "-o", output], cwd=flatpak)

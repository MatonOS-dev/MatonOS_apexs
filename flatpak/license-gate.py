#!/usr/bin/env python3
"""Fail the recipe if any declared component can require GPL/LGPL version 3."""
from pathlib import Path
import re
import sys

manifest = Path(__file__).with_name("licenses.tsv")
for line_no, line in enumerate(manifest.read_text().splitlines(), 1):
    if not line or line.startswith("#"):
        continue
    try:
        component, expression = line.split("|", 1)
    except ValueError:
        sys.exit(f"{manifest}:{line_no}: malformed license record")
    if re.search(r"\b(?:LGPL|GPL)-3\.0-(?:only|or-later)\b", expression, re.I):
        sys.exit(f"GPLv3 prohibited: {component}: {expression}")
print(f"License gate passed: {sum(1 for x in manifest.read_text().splitlines() if x and not x.startswith('#'))} components")

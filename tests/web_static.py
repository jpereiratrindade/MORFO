#!/usr/bin/env python3
"""Fast regression checks for browser JavaScript not covered by HTTP smoke tests."""

import re
import subprocess
import sys
from pathlib import Path


app = Path(sys.argv[1])
node = sys.argv[2]
source = app.read_text(encoding="utf-8")

subprocess.run([node, "--check", str(app)], check=True)

match = re.search(
    r"async function openRootMetricDetail\(metricId\)\{(?P<body>.*?)\n\}\nfunction renderRootMetricRecords",
    source,
    re.DOTALL,
)
assert match, "openRootMetricDetail function not found"
body = match.group("body")

for identifier in ("imageWidth", "imageHeight", "hasImageGeometry", "labelSize"):
    declaration = re.search(rf"\bconst\s+{identifier}\s*=", body)
    interpolation = re.search(rf"\$\{{{identifier}(?:\b|[.*])", body)
    assert declaration, f"{identifier} must be explicitly declared"
    if interpolation:
        assert declaration.start() < interpolation.start(), f"{identifier} used before declaration"

assert "Number.isFinite(imageWidth)" in body
assert "Number.isFinite(imageHeight)" in body
assert "${hasImageGeometry?" in body

print("Web static regression PASS: syntax and root metric geometry bindings")

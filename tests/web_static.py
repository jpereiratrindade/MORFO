#!/usr/bin/env python3
"""Fast regression checks for browser JavaScript not covered by HTTP smoke tests."""

import re
import subprocess
import sys
from pathlib import Path


app = Path(sys.argv[1])
index = Path(sys.argv[2])
profiles = Path(sys.argv[3])
node = sys.argv[4]
source = app.read_text(encoding="utf-8")
markup = index.read_text(encoding="utf-8")
profile_source = profiles.read_text(encoding="utf-8")

subprocess.run([node, "--check", str(app)], check=True)
subprocess.run([node, "--check", str(profiles)], check=True)

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

removed_evaluation_markers = (
    "/api/v1/evaluation/",
    "view-avaliacao",
    'data-view="avaliacao"',
    "MORFO-AVALIA",
    "Avaliação científica",
)
for marker in removed_evaluation_markers:
    assert marker not in source, f"removed evaluation marker remains in app.js: {marker}"
    assert marker not in markup, f"removed evaluation marker remains in index.html: {marker}"
    assert marker not in profile_source, f"removed evaluation marker remains in morfo-profiles.js: {marker}"

print("Web static regression PASS: syntax, metric geometry and removed evaluation UI")

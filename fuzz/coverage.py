#!/usr/bin/env python3
"""Line coverage per source file from gcov: coverage.py OBJDIR SOURCE..."""
import os
import re
import subprocess
import sys

objdir, sources = sys.argv[1], sys.argv[2:]
rows, total_hit, total_lines = [], 0, 0
for src in sources:
    raw = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + ".raw.o")
    out = subprocess.run(["gcov", "-n", "-o", raw, src], capture_output=True, text=True).stdout
    m = re.search(r"File '[^']*" + re.escape(os.path.basename(src)) + r"'\nLines executed:([\d.]+)% of (\d+)", out)
    if not m:
        rows.append((src, None, 0))
        continue
    pct, lines = float(m.group(1)), int(m.group(2))
    hit = round(pct * lines / 100)
    total_hit += hit
    total_lines += lines
    rows.append((src, pct, lines))
for src, pct, lines in sorted(rows, key=lambda r: (r[1] is None, -(r[1] or 0))):
    name = os.path.relpath(src, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    print(f"  {name:32} {'-' if pct is None else f'{pct:5.1f}%'} of {lines:5} lines")
print(f"  {'total':32} {100 * total_hit / max(total_lines, 1):5.1f}% of {total_lines:5} lines")

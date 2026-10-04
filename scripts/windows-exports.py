#!/usr/bin/env python3
"""Export the C host API without exporting Dynarmic's C++ implementation."""
# SPDX-License-Identifier: MPL-2.0
from pathlib import Path
import re
import subprocess
import sys

nm, output, *objects = sys.argv[1:]
result = subprocess.run([nm, "-g", "--defined-only", *objects], check=True,
                        text=True, stdout=subprocess.PIPE)
symbols = {}
for line in result.stdout.splitlines():
    fields = line.split()
    if len(fields) != 3:
        continue
    _, kind, name = fields
    if name.startswith(("_Z", "__", "_GLOBAL_")) or not re.fullmatch(r"[A-Za-z_]\w*", name):
        continue
    if kind in "TW":
        symbols[name] = ""
    elif kind in "BDRCV":
        symbols[name] = " DATA"
if not symbols:
    sys.exit("windows-exports: no C exports found")
Path(output).write_text("EXPORTS\n" + "".join(
    f"  {name}{symbols[name]}\n" for name in sorted(symbols)))

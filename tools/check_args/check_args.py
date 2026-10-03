#!/usr/bin/env python3
"""Usage: check_args.py <compile_commands.json> [path/to/check_args]"""

import json
import os
import shlex
import subprocess
import sys

database = json.load(open(sys.argv[1]))
checker = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "check_args")

lines = []
for entry in database:
    args = entry.get("arguments") or shlex.split(entry["command"])
    lines.append("\x1f".join([entry["directory"]] + args))

subprocess.run([checker], input="\n".join(lines) + "\n", text=True, check=True)

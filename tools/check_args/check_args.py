#!/usr/bin/env python3
"""Usage: check_args.py <compile_commands.json> [path/to/check_args]

Without the path the newest checker of the build directories out/* is run.
"""

import glob
import json
import os
import shlex
import subprocess
import sys

database = json.load(open(sys.argv[1]))
if len(sys.argv) > 2:
    checker = sys.argv[2]
else:
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    checkers = glob.glob(os.path.join(root, "out", "*", "check_args"))
    if not checkers:
        sys.exit("check_args isn't built - build it, or give its path")
    checker = max(checkers, key=os.path.getmtime)

records = []
for entry in database:
    args = entry.get("arguments") or shlex.split(entry["command"])
    fields = [str(len(args)), entry["directory"]] + args
    records.append("".join(field + "\0" for field in fields))

subprocess.run([checker], input="".join(records), text=True, check=True)

#!/usr/bin/env python3
"""Measures incremental rebuilds of a ninja project with and without the cache.

The build directory must be configured to use dist-clang as the compiler
launcher, e.g. for GN: cc_wrapper = "/path/to/dist-clang". The baseline is
measured in the same build directory with DC_DISABLED=1 - the client then runs
the compiler as is, so the commands stay the same and nothing is rebuilt
because of the switch.

Each scenario changes the source tree, rebuilds and restores the tree:
  touch-headers  - touch the given files without changing them: like a branch
                   switch there and back, or a regenerated header;
  touch-sources  - the same for sources;
  edit-source    - append a unique declaration to the given files: a real
                   change, that can't be cached;
  edit-comment   - append a comment line to the given files.

Example:
  incremental.py --build-dir ~/b/gn --target ydbd \\
      --scenario touch-headers ydb/core/base/blobstorage.h \\
      --scenario edit-source ydb/core/base/blobstorage.cpp
"""

import argparse
import os
import re
import subprocess
import sys
import time


def run_ninja(build_dir, target, env):
    start = time.monotonic()
    proc = subprocess.run(["ninja", "-C", build_dir, target], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)
    elapsed = time.monotonic() - start
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-20000:])
        raise RuntimeError("ninja failed")
    steps = re.findall(r"^\[\d+/(\d+)\]", proc.stdout, re.M)
    compiles = len(re.findall(r"^\[\d+/\d+\] (?:CXX|CC) ", proc.stdout, re.M))
    return elapsed, int(steps[-1]) if steps else 0, compiles


def read_stats(dist_clang):
    proc = subprocess.run([dist_clang, "--stats"], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True)
    stats = {}
    for line in proc.stdout.splitlines():
        name, value = line.split()
        stats[name] = int(value)
    return stats


def apply_scenario(kind, files):
    """Changes the files, returns a function to restore them."""
    saved = []
    for path in files:
        st = os.stat(path)
        with open(path, "rb") as f:
            saved.append((path, f.read(), st.st_atime_ns, st.st_mtime_ns))

    now = time.time_ns()
    for path, content, _, _ in saved:
        if kind == "edit-source":
            with open(path, "ab") as f:
                f.write(b"\nextern int dist_clang_bench_%d;\n" % now)
        elif kind == "edit-comment":
            with open(path, "ab") as f:
                f.write(b"\n// dist-clang benchmark\n")
        else:
            os.utime(path, ns=(now, now))

    def restore():
        # Restore the content with a fresh mtime - the outputs have to be
        # rebuilt anyway after an edit.
        for path, content, _, _ in saved:
            if kind.startswith("edit-"):
                with open(path, "wb") as f:
                    f.write(content)
    return restore


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--target", default="all")
    parser.add_argument("--source-dir", default=".",
                        help="the paths of scenario files are relative to it")
    parser.add_argument("--dist-clang", default="dist-clang",
                        help="client executable - for statistics")
    parser.add_argument("--scenario", nargs="+", action="append", default=[],
                        metavar=("KIND", "FILE"),
                        help="touch-headers|touch-sources|edit-source|edit-comment FILE...")
    parser.add_argument("--modes", default="baseline,cache",
                        help="comma-separated: baseline, cache")
    args = parser.parse_args()

    os.chdir(args.source_dir)

    # Start from the up-to-date build, and warm up the cache.
    print("Bringing the build up to date...", flush=True)
    run_ninja(args.build_dir, args.target, dict(os.environ))

    print("%-14s %-9s %9s %9s %8s %8s %8s" %
          ("scenario", "mode", "time, s", "compiles", "direct", "simple",
           "miss"))
    for scenario in args.scenario:
        kind, files = scenario[0], scenario[1:]
        for mode in args.modes.split(","):
            env = dict(os.environ)
            if mode == "baseline":
                env["DC_DISABLED"] = "1"

            restore = apply_scenario(kind, files)
            try:
                before = read_stats(args.dist_clang)
                elapsed, _, compiles = run_ninja(args.build_dir, args.target,
                                                 env)
                after = read_stats(args.dist_clang)
            finally:
                # Never leave the source tree modified.
                restore()

            def delta(name):
                return after.get(name, 0) - before.get(name, 0)

            print("%-14s %-9s %9.1f %9d %8d %8d %8d" %
                  (kind, mode, elapsed, compiles, delta("DIRECT_CACHE_HIT"),
                   delta("SIMPLE_CACHE_HIT"), delta("SIMPLE_CACHE_MISS")),
                  flush=True)

            # Rebuild the restored tree - not measured.
            run_ninja(args.build_dir, args.target, dict(os.environ))


if __name__ == "__main__":
    main()

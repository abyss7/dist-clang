#!/usr/bin/env python3
"""Checks, that crashes never put wrong outputs into a build.

The test runs its own daemon - with its own runtime directory, cache and
config in a temporary directory: the daemon of the user isn't touched, and
only the processes of the test are killed.

Two build directories of the same ninja project are needed: one compiles
through dist-clang, the other - with the plain compiler. Their objects must be
the same - so the build shouldn't put the paths of the build directory into
objects (e.g. no debug info, or with the paths remapped), and both directories
should be at the same depth relative to the sources.

Every iteration:
  1. changes the code of random sources;
  2. starts the build through dist-clang, and kills with SIGKILL a random
     victim at a random moment: the daemon, a compiler, a client or ninja;
  3. sometimes reverts the sources - while the killed compilations may still
     be running, and the reverted versions are restored from the cache;
  4. completes the build through dist-clang, and builds the reference;
  5. compares all objects of both builds.

The sources are restored at the end.

Example:
  crash_test.py --source-dir . --cache-build-dir out/Cache \\
      --reference-build-dir out/Reference --target All \\
      --files src/base/logging.cc src/daemon/emitter.cc --iterations 30
"""

import argparse
import filecmp
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

VICTIMS = ["daemon", "compiler", "client", "ninja"]


def isolate_environment():
    """Points the runtime directory, the cache and the config of the daemon
    into a new temporary directory - for this process and its children.
    Returns the directory."""
    # Short: the paths of the unix sockets are limited.
    root = tempfile.mkdtemp(prefix="dc-crash-", dir="/tmp")
    for name, subdir in (("XDG_RUNTIME_DIR", "run"),
                         ("XDG_CACHE_HOME", "cache"),
                         ("XDG_CONFIG_HOME", "config")):
        path = os.path.join(root, subdir)
        os.mkdir(path, 0o700)
        os.environ[name] = path
    return root


def remove_environment(root):
    """Stops the daemon of the test, and removes its directory."""
    daemons = pids("^[^ ]*dist-clangd ")
    for pid in daemons:
        os.kill(pid, signal.SIGTERM)
    for _ in range(100):
        if not any(os.path.exists("/proc/%d" % pid) for pid in daemons):
            break
        time.sleep(0.1)
    shutil.rmtree(root, ignore_errors=True)


def build(build_dir, target, env=None, check=True):
    proc = subprocess.run(["ninja", "-C", build_dir, target], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout[-20000:])
        raise RuntimeError("build failed in " + build_dir)
    return proc.returncode


def same_environment(pid):
    """Only the processes of this test - not the daemon of the user, that has
    the same name, but another runtime directory."""
    try:
        with open("/proc/%d/environ" % pid, "rb") as f:
            env = f.read().split(b"\0")
    except OSError:
        return False
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "").encode()
    return b"XDG_RUNTIME_DIR=" + runtime_dir in env


def pids(pattern):
    proc = subprocess.run(["pgrep", "-f", pattern], stdout=subprocess.PIPE,
                          text=True)
    return [int(pid) for pid in proc.stdout.split()
            if int(pid) != os.getpid() and same_environment(int(pid))]


def kill_random(victim):
    if victim == "daemon":
        candidates = pids("^[^ ]*dist-clangd ")
    elif victim == "compiler":
        # The compilers of the daemon - they are children of dist-clangd.
        daemons = set(pids("^[^ ]*dist-clangd "))
        candidates = []
        for pid in pids("clang"):
            try:
                with open("/proc/%d/stat" % pid) as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
            except (OSError, ValueError, IndexError):
                continue
            if ppid in daemons:
                candidates.append(pid)
    elif victim == "client":
        candidates = pids("^[^ ]*dist-clang ")
    else:
        candidates = []
    if candidates:
        pid = random.choice(candidates)
        try:
            os.kill(pid, signal.SIGKILL)
            return pid
        except ProcessLookupError:
            pass
    return None


MARKER = b"dist_clang_crash_test_"


def exit_on_signals():
    """The sources are restored in "finally" - on SIGTERM and SIGHUP too."""
    def handler(signum, frame):
        sys.exit(128 + signum)
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, handler)


def read_originals(paths):
    originals = {}
    for path in paths:
        with open(path, "rb") as f:
            originals[path] = f.read()
        if MARKER in originals[path]:
            sys.exit("%s is changed by an interrupted run - restore it first" %
                     path)
    return originals


def write_file(path, content):
    """Replaces the file at once: an interrupted write mustn't leave it
    half-written."""
    temp = "%s.dist-clang-%d" % (path, os.getpid())
    with open(temp, "wb") as f:
        f.write(content)
    shutil.copymode(path, temp)
    os.replace(temp, path)


def compare_objects(cache_dir, reference_dir):
    mismatches = []
    checked = 0
    for root, _, files in os.walk(reference_dir):
        for name in files:
            if not name.endswith(".o"):
                continue
            reference = os.path.join(root, name)
            cached = os.path.join(cache_dir,
                                  os.path.relpath(reference, reference_dir))
            checked += 1
            if not os.path.exists(cached) or \
                    not filecmp.cmp(cached, reference, shallow=False):
                mismatches.append(os.path.relpath(reference, reference_dir))
    return checked, mismatches


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--cache-build-dir", required=True)
    parser.add_argument("--reference-build-dir", required=True)
    parser.add_argument("--target", default="all")
    parser.add_argument("--files", nargs="+", required=True,
                        help="sources to change, relative to --source-dir")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--max-delay", type=float, default=5.0,
                        help="max seconds before the kill")
    parser.add_argument("--settle", type=float, default=10.0,
                        help="seconds to wait for stale compilations")
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    random.seed(args.seed)
    # The build directories may be relative to the current one.
    args.cache_build_dir = os.path.abspath(args.cache_build_dir)
    args.reference_build_dir = os.path.abspath(args.reference_build_dir)
    os.chdir(args.source_dir)
    originals = read_originals(args.files)
    exit_on_signals()
    failures = 0
    root = isolate_environment()
    print("The daemon of the test works in " + root, flush=True)

    try:
        build(args.cache_build_dir, args.target)
        build(args.reference_build_dir, args.target)

        for iteration in range(args.iterations):
            # Real changes of the code: a new function in random files. Some
            # iterations revert the files - to hit the cache.
            for path in random.sample(args.files,
                                      random.randint(1, len(args.files))):
                if random.random() < 0.3:
                    content = originals[path]
                else:
                    content = originals[path] + (
                        b"\nint %s%d_%d() { return %d; }\n" %
                        (MARKER, args.files.index(path), iteration,
                         random.randint(0, 1000)))
                write_file(path, content)

            victim = random.choice(VICTIMS)
            delay = random.uniform(0.1, args.max_delay)
            ninja = subprocess.Popen(
                ["ninja", "-C", args.cache_build_dir, args.target],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            killed = None
            try:
                ninja.wait(timeout=delay)
            except subprocess.TimeoutExpired:
                if victim == "ninja":
                    os.killpg(ninja.pid, signal.SIGKILL)
                    killed = ninja.pid
                else:
                    killed = kill_random(victim)
                ninja.wait()

            # Revert the sources, while the killed compilations may still be
            # running - like undoing an edit: the reverted versions are in the
            # cache, and are restored at once. The stale outputs of the killed
            # compilations mustn't get into the build.
            if killed and random.random() < 0.5:
                for path, content in originals.items():
                    write_file(path, content)

            # The build must complete after any crash.
            build(args.cache_build_dir, args.target)
            build(args.reference_build_dir, args.target)
            # Let the stale compilations finish, if any.
            if killed:
                time.sleep(args.settle)
            checked, mismatches = compare_objects(args.cache_build_dir,
                                                  args.reference_build_dir)
            status = "OK" if not mismatches else "MISMATCH"
            print("%3d: killed %-8s %-8s after %.1fs, %d objects: %s %s" %
                  (iteration, victim, killed or "-", delay, checked, status,
                   " ".join(mismatches[:5])), flush=True)
            if mismatches:
                failures += 1
    finally:
        for path, content in originals.items():
            write_file(path, content)
        remove_environment(root)

    print("%d of %d iterations with wrong objects" %
          (failures, args.iterations))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

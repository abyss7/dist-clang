#!/usr/bin/env python3
"""Real-Clang checks of chained PCH caching, with an isolated daemon and cache.

Example:
  python3 tools/pch_chain_test.py --suite basic --compiler clang++-18

Artifacts and complete compiler commands are retained in the printed directory.
Use --work-dir with a short path if TMPDIR exceeds the Unix socket path limit.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time


LAYERS = ("base", "group", "leaf")
SOURCES = {
    "base.h": "#pragma once\n#define BASE_VALUE 11\n"
              "#ifndef LATE_VALUE\n#define LATE_VALUE 31\n#endif\n"
              "template<int N> struct Number { static constexpr int value = N; };\n"
              "struct Base { int x = BASE_VALUE; };\n",
    "group.h": '#pragma once\n#include "base.h"\n#define GROUP_VALUE 7\n'
               "struct Group : Base { int y = GROUP_VALUE; };\n",
    "leaf.h": '#pragma once\n#include "group.h"\n#define LEAF_VALUE 3\n'
              "struct Leaf : Group { int z = LEAF_VALUE; };\n",
    "main.cc": "int Value() { Leaf a; return a.x + a.y + a.z + "
               "Number<BASE_VALUE + GROUP_VALUE + LEAF_VALUE>::value + LATE_VALUE; }\n",
    **{f"{layer}.cc": f'#include "{layer}.h"\n' for layer in LAYERS},
}


class Harness:
    def __init__(self, args):
        parent = Path(args.work_dir or tempfile.gettempdir())
        parent.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix=f"{args.suite}-", dir=parent))
        print(f"Artifacts: {self.root}", flush=True)
        self.work = self.root / "work"
        self.work.mkdir()
        self.log = (self.root / "commands.log").open("w")
        self.results = []
        self.daemon = None
        self.daemon_log = None
        binary_dir = Path(args.bin_dir).resolve()
        self.client = binary_dir / "dist-clang"
        self.server = binary_dir / "dist-clangd"
        runtime = self.root / "run"
        runtime.mkdir(mode=0o700)
        socket = runtime / "dist-clang.socket"
        if len(os.fsencode(socket)) >= 104:
            raise ValueError("Unix socket path too long; use a shorter --work-dir")
        home = self.root / "home"
        config = home / ".config" / "dist-clang"
        config.mkdir(parents=True)
        self.config = config / "daemon.conf"
        self.config.write_text(
            'emitter { threads: 2 preprocessing: ALWAYS }\n'
            f'cache {{ path: "{self.root / "cache"}" size: 536870912 '
            f'direct: true source_roots: "{self.work}/*" }}\n')
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith("DC_") and k not in
                    ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH")}
        self.env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
                        XDG_CACHE_HOME=str(home / ".cache"),
                        XDG_RUNTIME_DIR=str(runtime), DC_SOCKET_PATH=str(socket))

    def __enter__(self):
        self.daemon_log = (self.root / "daemon.log").open("w")
        self.daemon = subprocess.Popen(
            [str(self.server), "--managed", "--config=" + str(self.config)],
            env=self.env, stdout=self.daemon_log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if self.daemon.poll() is not None:
                    raise RuntimeError("Test daemon exited; see daemon.log")
                if self.run([self.client, "--stats", "--raw"], check=False).returncode == 0:
                    return self
                time.sleep(.05)
            raise RuntimeError("Test daemon startup timeout")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.daemon is not None:
            self.daemon.terminate()
            try:
                self.daemon.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait()
        if self.daemon_log is not None:
            self.daemon_log.close()
        self.log.close()

    def __exit__(self, *exc):
        self.close()

    def run(self, command, cwd=None, check=True):
        command = [str(x) for x in command]
        result = subprocess.run(command, cwd=cwd or self.root, env=self.env,
                                capture_output=True, text=True, timeout=180)
        self.log.write(f"cwd={cwd or self.root}\n$ {shlex.join(command)}\n"
                       f"exit={result.returncode}\n{result.stdout}{result.stderr}\n")
        self.log.flush()
        if check and result.returncode:
            raise AssertionError(f"{shlex.join(command)}\n{result.stderr}")
        return result

    def record(self, name, **values):
        self.results.append(dict(name=name, **values))
        (self.root / "results.json").write_text(json.dumps(self.results, indent=2))
        print(name, json.dumps(values, sort_keys=True), flush=True)

    def stats(self):
        result = self.run([self.client, "--stats", "--raw"])
        values = {}
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1].isdigit():
                values[parts[0]] = int(parts[1])
        return values

    def cached(self, name, command, cwd, hit=None, success=True):
        before = self.stats()
        result = self.run([self.client, *command], cwd, check=False)
        after = self.stats()
        delta = {key: after.get(key, 0) - before.get(key, 0) for key in
                 ("DIRECT_CACHE_HIT", "DIRECT_CACHE_MISS", "SIMPLE_CACHE_HIT",
                  "LOCAL_TASK_DONE", "CACHE_STORE_SKIPPED")}
        self.record(name, exit=result.returncode, stats=delta)
        assert (result.returncode == 0) == success, result.stderr
        assert delta["SIMPLE_CACHE_HIT"] == 0, (name, delta)
        if hit is not None:
            assert delta["DIRECT_CACHE_HIT"] == int(hit), (name, delta)
            if not hit:
                assert delta["DIRECT_CACHE_MISS"] == 1, (name, delta)
            if success and not hit:
                # A parser rejection would silently run outside the daemon.
                assert delta["LOCAL_TASK_DONE"] == 1, (name, delta)
        return delta


class Fixture:
    def __init__(self, harness, compiler, name, spelling="driver", relocatable=False):
        self.h = harness
        self.compiler = compiler
        self.name = name
        self.path = harness.work / name
        self.path.mkdir()
        for filename, contents in SOURCES.items():
            (self.path / filename).write_text(contents)
        self.spelling = spelling
        self.relocatable = relocatable
        self.macro = 31

    def flags(self):
        result = ["-std=c++20", "-O2"]
        if self.relocatable:
            result += ["-Xclang", "-relocatable-pch", "-isysroot", str(self.path) + "/",
                       "-ffile-prefix-map=" + str(self.path) + "/="]
        return result

    def pch_command(self, layer):
        result = [self.compiler, *self.flags(), "-x", "c++-header", "-c", layer + ".cc",
                  "-o", layer + ".pch", "-MD", "-MF", layer + ".d",
                  "-Xclang", "-fno-pch-timestamp"]
        index = LAYERS.index(layer)
        if index:
            parent = LAYERS[index - 1] + ".pch"
            if self.spelling == "driver":
                result += ["-include-pch", parent]
            else:
                result += [self.spelling, "-include-pch", self.spelling, parent]
        else:
            result += [f"-DLATE_VALUE={self.macro}"]
        return result

    def consumer_command(self, textual=False):
        result = [self.compiler, *self.flags(), "-c", "main.cc"]
        if textual:
            return result + ["-include", "leaf.h", f"-DLATE_VALUE={self.macro}", "-o", "reference.o"]
        return result + ["-include-pch", "leaf.pch", "-o", "main.o", "-MD", "-MF", "main.d"]

    def pipeline(self, stage, hits):
        for layer in LAYERS:
            self.h.cached(f"{self.name}/{stage}/{layer}", self.pch_command(layer),
                          self.path, hit=hits[layer])
        self.h.cached(f"{self.name}/{stage}/main", self.consumer_command(),
                      self.path, hit=hits["main"])
        self.reference()

    def reference(self):
        self.h.run(self.consumer_command(textual=True), self.path)
        assert (self.path / "main.o").read_bytes() == (self.path / "reference.o").read_bytes(), self.name


def basic(harness, compiler, index):
    for spelling in ("driver", "-Xclang", "-Xpreprocessor"):
        fixture = Fixture(harness, compiler, f"clang{index}-{spelling}", spelling)
        fixture.pipeline("cold", dict.fromkeys((*LAYERS, "main"), False))
        fixture.pipeline("warm", dict.fromkeys((*LAYERS, "main"), True))
        harness.record(fixture.name, status="PASS")


def invalidation(harness, compiler, index):
    fixture = Fixture(harness, compiler, f"clang{index}-invalidation")
    path = fixture.path
    all_hit = dict.fromkeys((*LAYERS, "main"), True)
    fixture.pipeline("cold", dict.fromkeys(all_hit, False))

    # Every result, including a PCH producer, depends on ALL its ancestors.
    for index, layer in enumerate((*LAYERS, "main")):
        deps = shlex.split((path / f"{layer}.d").read_text().replace("\\\n", ""))[1:]
        pch_deps = {Path(name).name for name in deps if name.endswith(".pch")}
        assert pch_deps == {name + ".pch" for name in LAYERS[:index]}, (layer, deps)
    original = {layer: (path / f"{layer}.pch").read_bytes() for layer in LAYERS}
    for suffix in ("*.pch", "*.o", "*.d"):
        for filename in path.glob(suffix):
            filename.unlink()
    fixture.pipeline("restore-all", all_hit)
    assert all((path / f"{layer}.pch").read_bytes() == original[layer] for layer in LAYERS)

    # Replacing an identical parent must not invalidate the cached children.
    (path / "base.h").write_text(SOURCES["base.h"])
    harness.run(fixture.pch_command("base"), path)
    assert (path / "base.pch").read_bytes() == original["base"]
    fixture.pipeline("identical-base", all_hit)

    def reset_headers():
        for layer in LAYERS:
            (path / f"{layer}.h").write_text(SOURCES[f"{layer}.h"])

    for index, layer in enumerate(LAYERS):
        reset_headers()
        fixture.pipeline(f"reset-before-{layer}", all_hit)
        old, new = {"base": ("11", "12"), "group": ("7", "8"), "leaf": ("3", "4")}[layer]
        (path / f"{layer}.h").write_text(SOURCES[f"{layer}.h"].replace(old, new))
        hits = {name: i < index for i, name in enumerate(LAYERS)}
        hits["main"] = False
        fixture.pipeline(f"edit-{layer}", hits)
        fixture.pipeline(f"warm-{layer}", all_hit)

    reset_headers()
    fixture.pipeline("original-headers", all_hit)
    # The old false-hit case: macro configuration changes only in the base;
    # the source headers and both children remain byte-for-byte identical.
    for macro in (32, 33):
        fixture.macro = macro
        fixture.pipeline(f"base-macro-{macro}", dict.fromkeys(all_hit, False))
        assert all((path / f"{layer}.pch").read_bytes() == original[layer]
                   for layer in ("group", "leaf"))
        fixture.pipeline(f"warm-macro-{macro}", all_hit)

    for index, missing in enumerate(LAYERS):
        filename = path / f"{missing}.pch"
        contents = filename.read_bytes()
        filename.unlink()
        try:
            harness.cached(f"{fixture.name}/missing-{missing}/main",
                           fixture.consumer_command(), path, hit=False, success=False)
            for child in LAYERS[index + 1:]:
                harness.cached(f"{fixture.name}/missing-{missing}/{child}",
                               fixture.pch_command(child), path, hit=False, success=False)
        finally:
            filename.write_bytes(contents)
        fixture.pipeline(f"restored-{missing}", all_hit)
    harness.record(fixture.name, status="PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("basic", "invalidation"), required=True)
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--bin-dir", default="out/Test")
    parser.add_argument("--work-dir")
    args = parser.parse_args()
    compilers = args.compiler or ["clang++-18"]
    with Harness(args) as harness:
        for index, compiler in enumerate(compilers):
            compiler = shutil.which(compiler)
            if not compiler:
                raise RuntimeError("Requested compiler not found")
            harness.record(f"compiler{index}", version=harness.run([compiler, "--version"]).stdout.splitlines()[0])
            {"basic": basic, "invalidation": invalidation}[args.suite](harness, compiler, index)
        harness.record("complete", suite=args.suite, status="PASS")


if __name__ == "__main__":
    main()

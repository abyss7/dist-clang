#!/usr/bin/env python3
"""Real-Clang checks of chained PCH caching, with an isolated daemon and cache.

Example:
  python3 tools/pch_chain_test.py --suite basic --compiler clang++-18

Suites: basic checks the three parent-argument spellings; invalidation checks
dependencies and restoration; relocation checks independent source roots; ydb
uses the compiler, flags and cwd of a PCH entry in --ydb-commands. The YDB suite
only builds temporary sources including real headers, not the YDB targets.

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


def relocation(harness, compiler, index):
    first = Fixture(harness, compiler, f"clang{index}-relocation-a", relocatable=True)
    all_hit = dict.fromkeys((*LAYERS, "main"), True)
    first.pipeline("cold", dict.fromkeys(all_hit, False))
    saved = {layer: (first.path / f"{layer}.pch").read_bytes() for layer in LAYERS}
    second = Fixture(harness, compiler, f"clang{index}-relocation-b", relocatable=True)
    hidden = first.path.with_name(first.path.name + "-hidden")
    first.path.rename(hidden)
    try:
        # The new root has only the sources. Restore each ancestor in order;
        # no compiler can accidentally read an artifact from the old root.
        second.pipeline("cross-root", all_hit)
        assert all((second.path / f"{layer}.pch").read_bytes() == saved[layer]
                   for layer in LAYERS)
        cached_object = (second.path / "main.o").read_bytes()
        harness.run(second.consumer_command(), second.path)
        assert (second.path / "main.o").read_bytes() == cached_object
        harness.record(second.name, status="PASS")
    finally:
        hidden.rename(first.path)

    absolute = Fixture(harness, compiler, f"clang{index}-absolute", relocatable=True)
    harness.cached(f"{absolute.name}/base", absolute.pch_command("base"),
                   absolute.path, hit=True)
    for layer in ("group", "leaf"):
        command = absolute.pch_command(layer)
        operand = command.index("-include-pch") + 1
        command[operand] = str(absolute.path / command[operand])
        for attempt in range(2):
            delta = harness.cached(f"{absolute.name}/{layer}/{attempt}",
                                   command, absolute.path, hit=False)
            # An absolute parent reference binds the child to this root. The
            # existing path guard must reject storing it as a shared artifact.
            assert delta["CACHE_STORE_SKIPPED"] == 1, (layer, delta)
    harness.record(absolute.name, status="PASS")


def ydb_command(database):
    """Keep a real PCH command's flags/cwd, replacing only its inputs/outputs."""
    for entry in json.loads(Path(database).read_text()):
        command = entry.get("arguments") or shlex.split(entry.get("command", ""))
        if not any(command[i:i + 2] == ["-x", "c++-header"]
                   for i in range(len(command) - 1)):
            continue
        cwd = Path(entry["directory"])
        source = (cwd / entry["file"]).resolve()
        compiler = shutil.which(command[0])
        if not compiler:
            raise ValueError(f"Compiler not found: {command[0]}")
        flags = []
        index = 1
        inputs = 0
        while index < len(command):
            flag = command[index]
            if flag in ("-o", "-MF", "-MT", "-MQ", "-x"):
                index += 2
                continue
            if flag in ("-c", "-MD", "-MMD", "-MP") or flag.startswith("-ftime-trace"):
                index += 1
                continue
            if not flag.startswith("-") and (cwd / flag).resolve() == source:
                inputs += 1
            else:
                flags.append(flag)
            index += 1
        if inputs != 1 or "-fno-pch-timestamp" not in flags:
            raise ValueError("Expected one PCH input with -fno-pch-timestamp")
        return compiler, flags, cwd, source
    raise ValueError("No -x c++-header command in the compilation database")


def ydb(harness, args):
    if not args.ydb_commands:
        raise ValueError("--suite ydb requires --ydb-commands")
    compiler, flags, cwd, source = ydb_command(args.ydb_commands)
    harness.record("ydb-command", compiler=compiler, cwd=str(cwd), source=str(source), flags=flags)
    path = harness.work / "ydb"
    path.mkdir()
    headers = {
        "base.h": '#pragma once\n#include <util/generic/vector.h>\n'
                  '#include <util/generic/string.h>\n#define DC_CHAIN_BASE 11\n',
        "group.h": '#pragma once\n#include "base.h"\n'
                   '#include <util/generic/hash.h>\n#define DC_CHAIN_GROUP 7\n',
        "leaf.h": '#pragma once\n#include "group.h"\n'
                  '#include <util/generic/algorithm.h>\n#define DC_CHAIN_LEAF 3\n',
        "main.cc": '#include <cstdio>\nint main() { std::printf("%zu\\n", '
                   'sizeof(TVector<int>) + sizeof(TString) + sizeof(THashMap<int, int>) '
                   '+ DC_CHAIN_BASE + DC_CHAIN_GROUP + DC_CHAIN_LEAF); }\n',
    }
    for name, contents in headers.items():
        (path / name).write_text(contents)
    for layer in LAYERS:
        (path / f"{layer}.cc").write_text(f'#include "{layer}.h"\n')

    def command(layer, textual=False):
        result = [compiler, *flags]
        if layer in LAYERS:
            result += ["-x", "c++-header"]
        result += ["-c", str(path / f"{layer}.cc")]
        output = path / ("reference.o" if textual else f"{layer}.o" if layer == "main" else f"{layer}.pch")
        result += ["-o", str(output), "-MD", "-MF", str(output.with_suffix(".d"))]
        if layer in LAYERS:
            index = LAYERS.index(layer)
            if index:
                result += ["-include-pch", os.path.relpath(path / f"{LAYERS[index - 1]}.pch", cwd)]
        elif textual:
            result += ["-include", str(path / "leaf.h")]
        else:
            result += ["-include-pch", os.path.relpath(path / "leaf.pch", cwd)]
        return result

    def pipeline(stage, hit):
        for layer in (*LAYERS, "main"):
            harness.cached(f"ydb/{stage}/{layer}", command(layer), cwd, hit=hit)

    linker = shutil.which(args.linker)
    if not linker:
        raise ValueError(f"Linker driver not found: {args.linker}")
    lld = Path(compiler).with_name("ld.lld")
    if not lld.is_file():
        raise ValueError(f"Expected the YDB toolchain's linker at {lld}")

    def output(object_name, executable):
        # sizeof exercises real container layouts without needing YDB's built
        # libraries. Link the tiny object against the host runtime only.
        # Use the matching lld: the host's older GNU ld may not understand
        # the real command's -gz=zstd debug sections.
        harness.run([linker, "-fuse-ld=" + str(lld), path / object_name,
                     "-Wl,--gc-sections", "-o", path / executable])
        return harness.run([path / executable]).stdout

    pipeline("cold", False)
    harness.run(command("main", textual=True), cwd)
    expected = output("reference.o", "reference")
    assert output("main.o", "cached") == expected
    saved = {filename.name: filename.read_bytes() for filename in path.iterdir()
             if filename.suffix in (".pch", ".o", ".dwo") and not filename.name.startswith("reference.")}
    pipeline("warm", True)
    for name in saved:
        (path / name).unlink()
    for filename in path.glob("*.d"):
        filename.unlink()
    pipeline("restore-all", True)
    assert all((path / name).read_bytes() == contents for name, contents in saved.items())
    assert output("main.o", "restored") == expected
    harness.record("ydb", status="PASS", reference_output=expected.strip(),
                   restored_artifacts=sorted(saved))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("basic", "invalidation", "relocation", "ydb"), required=True)
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--bin-dir", default="out/Test")
    parser.add_argument("--work-dir")
    parser.add_argument("--ydb-commands", help="YDB compile_commands.json with a PCH command")
    parser.add_argument("--linker", default="clang++-18", help="host compiler driver for the YDB smoke program")
    args = parser.parse_args()
    compilers = args.compiler or ["clang++-18"]
    with Harness(args) as harness:
        if args.suite == "ydb":
            ydb(harness, args)
        else:
            for index, compiler in enumerate(compilers):
                compiler = shutil.which(compiler)
                if not compiler:
                    raise RuntimeError("Requested compiler not found")
                harness.record(f"compiler{index}", version=harness.run([compiler, "--version"]).stdout.splitlines()[0])
                {"basic": basic, "invalidation": invalidation,
                 "relocation": relocation}[args.suite](harness, compiler, index)
        harness.record("complete", suite=args.suite, status="PASS")


if __name__ == "__main__":
    main()

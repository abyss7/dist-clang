**DistClang** is a compilation cache and distributed compilation system for Clang. It intercepts compiler invocations, and reuses the results of previous compilations - both locally and from remote hosts.

The project consists of 2 executables:

- **dist-clang** is a thin client - it replaces the invocation of the compiler, and sends the command line to the daemon. If the daemon isn't available, the client runs the compiler as is.
- **dist-clangd** is a daemon. The local daemon (**emitter**) classifies the compiler flags, looks up the cache, compiles on a cache miss and stores the result. Remote daemons (**absorbers**) may compile preprocessed sources for emitters.

# How to build

Clone with submodules (`git clone --recurse-submodules`, or `git submodule update --init` after cloning). You need [GN](https://gn.googlesource.com/gn/), Ninja, Clang with C++20 support and zlib headers.

    build/configure 'clang_cc="clang-18"' 'clang_cxx="clang++-18"'
    ninja -C out/Release All

The resulting files `dist-clang` and `dist-clangd` are located in `out/Release` - they should be placed in the same directory.

To build and run the tests:

    ninja -C out/Test Tests
    build/run_all_tests

# How to use

Run the compiler through the client:

    dist-clang clang++ -c foo.cc -o foo.o

The first invocation starts the daemon in background. It exits within seconds after its executable is replaced, e.g. by a new version: the next invocation starts the new one. For build systems:

- GN: use the client as a prefix of the compiler in the toolchain, e.g. `cxx = "dist-clang clang++"`.
- CMake: `-DCMAKE_C_COMPILER_LAUNCHER=dist-clang -DCMAKE_CXX_COMPILER_LAUNCHER=dist-clang`.

Alternatively make symlinks `clang` and `clang++` to `dist-clang`, and provide the real compiler via `DC_CLANG_PATH` environment variable, or via `.distclang` file in any parent directory:

    path: "/usr/bin/clang-18"

Commands, which can't be cached (linking, preprocessing, unknown inputs, etc.), are run as is.

If the build system re-stats outputs after commands - like Ninja with `restat = 1` for the compilation rules - use `dist-clang --restat clang++ ...`: then the outputs, that don't change after a cache hit or a compilation, are kept untouched with their old modification time, and the dependent targets aren't relinked. E.g. touching a header, or changing a comment in it, doesn't relink anything. Don't use it without re-stating: the build system would consider such outputs as outdated forever.

To measure a build as without the cache, but still fill it, run it with `DC_RECACHE=1` in the environment - e.g. `DC_RECACHE=1 ninja`, nothing to regenerate - or with `dist-clang --recache clang++ ...`: the compilations don't look up the cache, and don't preprocess the sources, but their results are stored for the next builds. The same for the compilations with `-ftime-trace`: only the compilation itself gives the trace, while the flag doesn't get into the key - unless the cache stores the traces.

With `store_time_traces: true` in the cache configuration the traces are stored with the results, and the hits restore them - e.g. to look for the expensive headers and templates with every incremental build, without the full one: the trace next to the object is always of the compilation, that produced it - also after a branch switch. Such hits need the entries with the traces of the same options (`-ftime-trace-granularity`, `-ftime-trace-verbose`): the others are compiled again, and get the traces. The times in the traces are the ones of those compilations - measure the build itself with `DC_RECACHE=1`. The traces take a part of the cache, see "Time traces" in `--stats`; without the option they're removed after the start of the daemon, and the entries stay.

Show the statistics of the daemon - grouped, or with `--raw` as `NAME value` lines for scripts:

    dist-clang --stats [--raw]

## Tags

The cache entries may be tagged with `--tag=<tag>` (repeatable): e.g. with the git branch, to see, how much space every branch takes, and to remove the entries of a finished branch. For GN, put the command into the launcher - the shell runs it for every compilation, while the command line stays the same for Ninja, so a branch switch doesn't rebuild anything by itself. The branch, or the short hash of the commit for the detached HEAD, of the sources, where `gn gen` runs:

    cc_wrapper = "dist-clang --restat --tag=\$\$(cd " + rebase_path("//") + " 2>/dev/null && { git symbolic-ref --short -q HEAD || git rev-parse --short HEAD; } 2>/dev/null)"

Here the toolchain passes `cc_wrapper` to the compiler tools as `command_launcher`: GN doesn't put the launcher into `compile_commands.json` - otherwise clangd and other tools get the options of dist-clang, and the command substitution, as the arguments of the compiler. GN doesn't escape the launcher for Ninja, so `$` is written as `$$`. If the toolchain puts `cc_wrapper` into the command itself, GN escapes it, and a single `\$` is enough.

The empty tags are ignored, any whitespace separates several tags - also in one `--tag`. The entries get the tags, when they are stored or found in the cache. Show the sizes of the tags and their overlaps; every output counts once. The size "in builds" is linked with build directories: it takes no space in the cache, and isn't removed with the tag. The "exclusive" size is used only by the tag, and the "freeable" one is what `--forget-tag` frees now:

    dist-clang --tag-stats

Remove a tag: the entries, that only it uses, are removed - except the ones, that are linked with build directories - and the other entries lose the tag. Without `--force` it only shows, what would be removed:

    dist-clang --forget-tag=<tag> [--force]

The same for the entries without tags - e.g. stored before the tags were used:

    dist-clang --forget-untagged [--force]

The objects, that weren't recompiled after a branch switch, keep only the tags of the previous branches - so a finished branch may be removed only after the other branches are built.

A tag may have its own rules - describe them in the cache configuration and pass the tag to the build with `--tag=<tag>`, with other tags too, if needed. The rules are independent of each other:

- `isolated: true` - the lookups of the build with the tag find only the entries with the tag, while other builds find them too: the outputs are stored once. The direct cache is isolated too: its manifests are shared, but the lookups with the tag see only the headers and the entries, that the builds with the tag have stored. The outputs of such entries aren't compressed: the hits don't get slower with the time.
- `quota: <bytes>` - the own size of the tag, `0` - unlimited. The entries of the tag don't count in the size of the cache, and others don't evict them. When the tag doesn't fit, its oldest entries by the uses of the tag - not by the hits of others - lose the tag, and follow the rules of their other tags then. Without it, the entries follow the common rules.

E.g. a reference measurement of the incremental build needs a cache, that only this build uses and fills, of a fixed size:

    tags { name: "bench" isolated: true quota: 21474836480 }  # 20 GiB

Take a new name for such tag: the entries, that got the tag before it became isolated, were stored by other builds - the isolated build doesn't see them through the direct cache, but finds them after the preprocessing. A release branch may keep its entries without isolation - `tags { name: "release-1.0" quota: 0 }` - until `--forget-tag` removes them.

## Environment variables

- `DC_DISABLED` - run the compiler as is, bypassing the daemon.
- `DC_NO_AUTOSTART` - don't start the daemon automatically.
- `DC_SOCKET_PATH` - path to the daemon's socket: `$XDG_RUNTIME_DIR/dist-clang.socket` by default.
- `DC_CLANG_PATH` - the real compiler, when the client is called via a symlink.
- `DC_TAGS` - the tags of the compilations, separated by whitespace - the same as `--tag`; `--tag` adds to them.
- `DC_RESTAT=1`, `DC_RECACHE=1` - the same as `--restat` and `--recache`.

# How the cache works

The daemon classifies the flags of the compiler driver: the flags, that affect only the preprocessing (`-I`, `-D`, etc.), the flags, that don't affect the result at all (dependency file generation, `-ftime-trace`), and all others - unknown flags are considered to affect the result. The cache key includes the identity of the compiler (the hash of its binary and of its default configuration), the flags and the parts of the environment that affect the compilation. There are 2 modes of the cache:

- **direct**: the key is the source file, the flags and the contents of all included headers - from the dependency file of the previous compilation. A hit doesn't run the compiler at all. It also checks, that no new file appeared in the include directories (`-iquote`, `-I`, `-isystem`, `-idirafter` and the directory of the source), which the compiler would find instead of a used header. The implicit include directories of the compiler aren't checked, and neither are the directories of the including headers. The results of compilations, that use `__DATE__`, `__TIME__` or `__TIMESTAMP__`, aren't stored for the direct mode.
- **simple**: the key is the preprocessed source and the flags, except the preprocessing-only ones. It catches the changes, that don't affect the preprocessed source, e.g. touched or regenerated headers, comments at the end of a file, or unused macro definitions.

The preprocessing costs about a quarter of the compilation, so on a miss of the direct cache the daemon doesn't wait for it (`preprocessing: WHEN_IDLE` in the emitter configuration, by default): it compiles at once, and preprocesses at the same time, while there are idle CPUs. If the preprocessed source is found in the simple cache first, the compilation is canceled. The result is stored for both caches. The sources, that are new - e.g. on the first build - or edited, aren't preprocessed at all: their preprocessed versions can't be found. Then the next change of the flags, like a new `-D`, isn't found by the preprocessed source yet - only the following ones. `preprocessing: ALWAYS` preprocesses before the compilation, `NEVER` uses only the direct cache. With remotes the sources are always preprocessed.

The results include the object file, the split DWARF file (`-gsplit-dwarf`), the dependency file and the compiler's diagnostics. The flags, that change only the format of the diagnostics (`-fcolor-diagnostics`, `-fmessage-length=`, etc.), aren't a part of the key.

The warning flags (`-W*`, `-w`, `-pedantic`) are a part of the key by default. With `recheck_warnings: true` in the emitter configuration they aren't: if a cached result was produced with other warnings, the daemon checks the source with `-fsyntax-only` and the current flags, and takes the object file from the cache - so changing warnings doesn't cause the code generation. The warnings of the backend (e.g. `-Wframe-larger-than`) are missed in this case.

To catch the inputs, that affect the result, but aren't a part of the key, set `verify_hits_percent` in the emitter configuration: this percent of the cache hits is compiled again in background with the lowest priority, and the results are compared with the cached ones. A mismatch is reported to the log and to the statistics (`HIT_VERIFICATION_FAILED`), and the wrong entry is removed. The compilations, that record the command line in the debug info, aren't verified, since the path of the output differs - pass `-gno-record-command-line` to verify them.

The object and split DWARF files are stored uncompressed, and are put into and out of the cache without copying: as copy-on-write clones on filesystems that support them (btrfs, XFS, APFS), or as hard links otherwise. The contents of the linked files are checked on every cache hit - a file modified in place through another link is not taken from the cache. A cache file is hard-linked to one place outside of the cache at most (`max_output_links: 1` by default), and other places - e.g. other build directories with the same outputs - get copies: the build systems don't expect, that a modification of their output in place, e.g. with `cp` over it, changes the outputs of other builds, and their modification time too. `max_output_links: 0` removes the limit. The copy-on-write clones don't have this problem. The outputs, that aren't linked with build directories anymore - e.g. the results of other branches - and weren't used for `compress_after` seconds (an hour by default), are compressed in background; they are uncompressed again, when they are used. Set `link_outputs: false` in the cache configuration to store compressed copies instead - with `compression: ZSTD` (by default) or `NONE`.

The object and split DWARF files are stored by the hashes of their contents: the same output of several entries - e.g. compiled with other warnings, or with a definition, that doesn't affect the source - takes the space once.

The size limit of the cache applies only to the files, that aren't linked with build directories: the cache doesn't take any additional space for the outputs of the current builds. Such entries are never removed. Other entries are removed, when the cache exceeds the limit, or when the disk has less than `min_free_percent` (5% by default) of free space: the entries, that were never taken from the cache - e.g. the results of one-off edits - go first, then the least recently used ones.

## Format of the cache

The file `format` in the root of the cache keeps its generation and revision. The generation changes with the incompatible changes of the layout: a new daemon removes the cache of an older generation at the start - it's moved aside at once, and removed in background - and an old daemon doesn't use the cache of a newer generation: it compiles without the cache, and `dist-clang --stats` shows `CACHE_INCOMPATIBLE 1`. The revision changes with the compatible changes: the older daemons of the same generation still use the cache. Nothing is migrated: the entries of other versions are just removed, and an incompatible change of the format bumps the generation - it's cheaper to rebuild than to migrate. Only the files of the cache are removed: the other files in its directory stay. The cache without the file is of the first generation.

The daemon checks all files of the cache only after a crash, or when the index is created anew: the file `running` in the root of the cache exists, while the daemon works. The stop on `SIGTERM` or `SIGINT` is normal: the recent accesses are saved to the index, and the running compilations aren't waited for - the clients compile without the daemon.

## Several checkouts of the same sources

The cache key includes the paths from the flags and from the preprocessed source. To share the cache between several checkouts of the same sources - e.g. git worktrees - list them in `source_roots` of the cache configuration: the root of the compiled source is replaced with a placeholder in the key, both as an absolute path and as a path relative to the build directory. It works only for relocatable compilations, that remap the paths of the sources in the results: with `-fmacro-prefix-map` (or `-ffile-prefix-map`) for `__FILE__`, and, if the debug info is on, with `-fdebug-prefix-map` and `-fdebug-compilation-dir` too.

# How to configure the daemon

Without a configuration the daemon uses the defaults: the cache in `~/.cache/dist-clang` of 20 GiB. To change them, copy `install/daemon.conf.example` to `~/.config/dist-clang/daemon.conf`: the fields of the file replace only the same defaults. The format is the text protobuf: `dist-clang --help-config` describes all fields. The daemon reads the file at the start: `dist-clang --stop` stops it to apply the changes, and the next compilation starts it again. The daemon of root, that serves other users, is stopped only by root.

# Benchmarks

`tools/bench/incremental.py` measures incremental rebuilds of a ninja project with and without the cache.

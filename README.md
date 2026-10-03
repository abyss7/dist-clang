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

The first invocation starts the daemon in background. For build systems:

- GN: use the client as a prefix of the compiler in the toolchain, e.g. `cxx = "dist-clang clang++"`.
- CMake: `-DCMAKE_C_COMPILER_LAUNCHER=dist-clang -DCMAKE_CXX_COMPILER_LAUNCHER=dist-clang`.

Alternatively make symlinks `clang` and `clang++` to `dist-clang`, and provide the real compiler via `DC_CLANG_PATH` environment variable, or via `.distclang` file in any parent directory:

    path: "/usr/bin/clang-18"

Commands, which can't be cached (linking, preprocessing, unknown inputs, etc.), are run as is.

If the build system re-stats outputs after commands - like Ninja with `restat = 1` for the compilation rules - use `dist-clang --restat clang++ ...`: then the outputs, that don't change after a cache hit or a compilation, are kept untouched with their old modification time, and the dependent targets aren't relinked. E.g. touching a header, or changing a comment in it, doesn't relink anything. Don't use it without re-stating: the build system would consider such outputs as outdated forever.

Show the statistics of the daemon:

    dist-clang --stats

## Environment variables

- `DC_DISABLED` - run the compiler as is, bypassing the daemon.
- `DC_NO_AUTOSTART` - don't start the daemon automatically.
- `DC_SOCKET_PATH` - path to the daemon's socket: `$XDG_RUNTIME_DIR/dist-clang.socket` by default.
- `DC_CLANG_PATH` - the real compiler, when the client is called via a symlink.

# How the cache works

The daemon classifies the flags of the compiler driver: the flags, that affect only the preprocessing (`-I`, `-D`, etc.), the flags, that don't affect the result at all (dependency file generation, `-ftime-trace`), and all others - unknown flags are considered to affect the result. The cache key includes the identity of the compiler (the hash of its binary and of its default configuration), the flags and the parts of the environment that affect the compilation. There are 2 modes of the cache:

- **direct**: the key is the source file, the flags and the contents of all included headers - from the dependency file of the previous compilation. A hit doesn't run the compiler at all. It also checks, that no new file appeared in the include directories (`-iquote`, `-I`, `-isystem`, `-idirafter` and the directory of the source), which the compiler would find instead of a used header. The implicit include directories of the compiler aren't checked, and neither are the directories of the including headers. The results of compilations, that use `__DATE__`, `__TIME__` or `__TIMESTAMP__`, aren't stored for the direct mode.
- **simple**: the key is the preprocessed source and the flags, except the preprocessing-only ones. It catches the changes, that don't affect the preprocessed source, e.g. touched or regenerated headers, comments at the end of a file, or unused macro definitions.

The results include the object file, the split DWARF file (`-gsplit-dwarf`), the dependency file and the compiler's diagnostics. The flags, that change only the format of the diagnostics (`-fcolor-diagnostics`, `-fmessage-length=`, etc.), aren't a part of the key.

The warning flags (`-W*`, `-w`, `-pedantic`) are a part of the key by default. With `recheck_warnings: true` in the emitter configuration they aren't: if a cached result was produced with other warnings, the daemon checks the source with `-fsyntax-only` and the current flags, and takes the object file from the cache - so changing warnings doesn't cause the code generation. The warnings of the backend (e.g. `-Wframe-larger-than`) are missed in this case.

To catch the inputs, that affect the result, but aren't a part of the key, set `verify_hits_percent` in the emitter configuration: this percent of the cache hits is compiled again in background with the lowest priority, and the results are compared with the cached ones. A mismatch is reported to the log and to the statistics (`HIT_VERIFICATION_FAILED`), and the wrong entry is removed. The compilations, that record the command line in the debug info, aren't verified, since the path of the output differs - pass `-gno-record-command-line` to verify them.

The object and split DWARF files are stored uncompressed, and are put into and out of the cache without copying: as copy-on-write clones on filesystems that support them (btrfs, XFS), or as hard links otherwise. The contents of the linked files are checked on every cache hit - a file modified in place through another link is not taken from the cache. Set `link_outputs: false` in the cache configuration to store compressed copies instead - with `compression: ZSTD` (by default), `SNAPPY` or `NONE`.

The size limit of the cache applies only to the files, that aren't linked with build directories: the cache doesn't take any additional space for the outputs of the current builds. Such entries are never removed. Other entries are removed, when the cache exceeds the limit, or when the disk has less than `min_free_percent` (5% by default) of free space: the entries, that were never taken from the cache - e.g. the results of one-off edits - go first, then the least recently used ones.

## Several checkouts of the same sources

The cache key includes the paths from the flags and from the preprocessed source. To share the cache between several checkouts of the same sources - e.g. git worktrees - list them in `source_roots` of the cache configuration: the root of the compiled source is replaced with a placeholder in the key, both as an absolute path and as a path relative to the build directory. It works only for relocatable compilations, that remap the paths of the sources in the results: with `-fmacro-prefix-map` (or `-ffile-prefix-map`) for `__FILE__`, and, if the debug info is on, with `-fdebug-prefix-map` and `-fdebug-compilation-dir` too.

# How to configure the daemon

Without a configuration the daemon uses the defaults: the cache in `~/.cache/dist-clang` of 20 GiB. To change them, copy `install/daemon.conf.example` to `~/.config/dist-clang/daemon.conf`. The format is the text protobuf, see `src/daemon/configuration.proto`.

# Benchmarks

`tools/bench/incremental.py` measures incremental rebuilds of a ninja project with and without the cache.

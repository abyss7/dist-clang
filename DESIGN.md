Architecture of dist-clang
==========================

The user-facing behaviour - the modes of the cache, the flags, the
configuration - is in [README.md](README.md). This page maps it onto the code.

Client
------

`dist-clang` (`tools/clang/main.cc`, `src/client/`) replaces the compiler in
the build: either as a prefix (`dist-clang clang++ ...`), or as a link named
like the compiler with `DC_CLANG_PATH`.

1.  `ParseInvocation()` (`invocation.cc`) tells a compilation from a command
    of the client (`--stats`, `--forget-tag=` etc.) and takes its own flags
    (`--tag=`, `--restat`, `--recache`).
2.  `Configuration` (`configuration.cc`) reads the optional config file of the
    client and the environment.
3.  `DoMain()` (`clang.cc`) sends the daemon the raw command line, the whole
    environment, the current directory and the user id. The client doesn't
    parse the flags of the compiler: the daemon does it.
4.  The client prints the diagnostics from the daemon and exits with its
    code. If the daemon can't handle the compilation, the client runs the
    compiler itself - and, if the daemon isn't running, starts it in
    background for the next compilations (`autostart.cc`).

Daemon
------

`dist-clangd` (`src/daemon/daemon_main.cc`) runs one of the roles from its
configuration (`configuration.proto`). For local builds it's the emitter.

The emitter (`emitter.cc`) has three queues with their own workers:

1.  `HandleNewMessage()` and `PrepareFlags()`: the arguments are classified by
    `ParseDriverArgs()` (`driver_args.cc`) - what affects the preprocessing,
    the code generation, only the diagnostics, nothing. Unknown flags are
    considered to affect the result; unsupported compilations are refused, and
    the client compiles them itself. The compiler is identified by the hash of
    its binary and its defaults (`compiler_identity.cc`).
2.  `DoCheckCache()`: the direct cache is looked up by the source, the flags
    and the headers from the previous compilation. On a miss the source is
    preprocessed - before the compilation or at the same time with it, see
    `Speculation` - and the simple cache is looked up by the preprocessed
    source.
3.  `DoLocalExecute()` compiles, `DoRemoteExecute()` sends the preprocessed
    source to an absorber. The result is stored for both caches.

The outputs of the same path are serialized by `OutputLocks`, and
`PreviousOutput` (`outputs.cc`) keeps the unchanged outputs with their old
times for `--restat`.

The other roles are for the remote compilations: the absorber (`absorber.cc`)
compiles the preprocessed sources from emitters; the coordinator tells the
emitters the absorbers; the collector gathers the statistics. The remote
compilations don't work now.

Cache
-----

`FileCache` (`src/cache/file_cache.cc`) keeps everything in one directory:

-   `<h>/<h>/<hash>.manifest` - the entries of the simple cache
    (`manifest.proto`: the hashes of the outputs, the compilation time), with
    the dependency file and the diagnostics beside them; and the manifests of
    the direct cache: the sets of headers, that a source used.
-   `direct.sqlite` - the direct cache: the key with the contents and the paths
    of the headers leads to an entry of the simple cache.
-   `blobs/` - the object and split DWARF files by the hashes of their
    contents (`blob_store.cc`): hard-linked or cloned with the build
    directories, or compressed.
-   `index.sqlite` - the sizes, the times of use and the blobs of the entries:
    for the eviction. `tags.sqlite` - the tags of the entries
    (`tag_index.cc`). `ghosts.sqlite` - the removed entries, to count the
    losses (`ghost_index.cc`).
-   `format`, `running`, `lock` - the generation of the layout, the mark of a
    crash, and the lock of the only daemon.

The lookups and the stores run in the workers of the emitter. The single
cleaner thread applies the accesses to the index, evicts, compresses the cold
blobs and counts their references. `FileHashCache` memoizes the hashes of the
headers by the times of their files; `DirectoryListings` checks, that no new
file in the include directories shadows a used header.

Other parts
-----------

-   `src/base/` - files, processes, logging, threads.
-   `src/net/` - the event loop, the unix and TCP connections, the protocol
    (`universal.proto`).
-   `src/perf/` - the statistics, that `dist-clang --stats` shows.
-   `tools/stress/`, `tools/bench/` - the crash test and the benchmark of
    incremental rebuilds.

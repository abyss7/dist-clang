# File Cache Design Document
The DistClang uses two types of cache: _simple_ and _direct_.

## Simple cache
Simple cache maps the Clang version, filtered command line and preprocessed sources to the outputs.

### Implementation
This cache uses a two-level directory hash-table as a main backend. The hash algorithm is the _MurmurHash3_. All the filenames for a single entry contain the hash of a tuple. The file extension is used to distinguish outputs. The current supported outputs are:
- object file (`.o`),
- dependencies file (`.d`),
- stderr from compiler (`.err`).

Also there is a `.manifest` file that contains some meta information about a cache entry, like:
- manifest version,
- snappy-compressed.

For more details see [manifest.proto](https://github.com/abyss7/dist-clang/blob/master/src/cache/manifest.proto).

The cache replacement policy is an exact LRU - a file modification time is used to track the usage. The index of all entries is stored in an in&#8209;memory SQLite database. To speed up the initialization of index the SQLite database may be stored on disk.

## Direct cache
Direct cache maps the Clang version, more strictly filtered command line and original sources to the _simple_ cache entry. The main difference is that direct cache can be used only on the same machine where it is build, but it gives a major performance boost - compared to the simple cache.

### Implementation
Uses a LevelDB database to store the mapping from the _direct_ hash to the _simple_ hash. Also the _direct_ entry is represented by `.manifest` file in the same two-level directory structure - from the simple cache.

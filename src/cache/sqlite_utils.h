#pragma once

#include <base/types.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Opens the database. If it's corrupted - e.g. by a crash of the system - the
// database is removed and created anew: it's only an index of the cache, and
// the missing entries are just cache misses.
sqlite3* OpenDatabase(const String& path);

}  // namespace cache
}  // namespace dist_clang

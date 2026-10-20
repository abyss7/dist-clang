#pragma once

#include <base/types.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Opens the database. If it's corrupted - e.g. by a crash of the system - the
// database is removed and created anew: it's only an index of the cache, and
// the missing entries are just cache misses.
// Sets |created|, if the database is new: it didn't exist, or was broken.
// The |full_check| reads the whole database - for the starts after a crash.
// Otherwise only the header is checked.
sqlite3* OpenDatabase(const String& path, bool* created = nullptr,
                      bool full_check = true);

}  // namespace cache
}  // namespace dist_clang

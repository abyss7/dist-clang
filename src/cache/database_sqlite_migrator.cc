#include <cache/database_sqlite.h>

namespace dist_clang {
namespace cache {

bool SQLite::Migrate() const {
  // Nothing to migrate right now.
  return GetVersion() == kSQLiteVersion;
}

}  // namespace cache
}  // namespace dist_clang

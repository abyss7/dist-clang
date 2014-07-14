#include <file_cache/database.h>

#include <base/logging.h>

#include <base/using_log.h>

namespace dist_clang {
namespace file_cache {

Database::Database(const String& path, const String& name)
    : path_(path + "/" + name + ".kch") {}

bool Database::Set(const String& key, const String& value) {
  using namespace kyotocabinet;

  HashDB db;
  if (!db.open(path_, HashDB::OWRITER | HashDB::OCREATE) ||
      !db.set(key, value) || !db.close()) {
    LOG(DB_ERROR) << db.error().name() << ": " << db.error().message();
    return false;
  }

  return true;
}

bool Database::Get(const String& key, String* value) const {
  using namespace kyotocabinet;

  HashDB db;
  if (!db.open(path_, HashDB::OREADER) || !db.get(key, value) || !db.close()) {
    LOG(DB_ERROR) << db.error().name() << ": " << db.error().message();
    return false;
  }

  return true;
}

}  // namespace file_cache
}  // namespace dist_clang

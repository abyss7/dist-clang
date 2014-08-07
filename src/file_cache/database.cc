#include <file_cache/database.h>

#include <base/logging.h>

#include <base/using_log.h>

namespace dist_clang {
namespace file_cache {

Database::Database(const String& path, const String& name)
    : path_(path + "/" + name + ".kch") {
  LOG(DB_INFO) << "Database is created on path " << path_;
}

bool Database::Set(const String& key, const String& value) {
  using namespace kyotocabinet;

  HashDB db;
  if (!db.open(path_, HashDB::OWRITER | HashDB::OCREATE)) {
    LOG(DB_ERROR) << "Failed to open database with error: "
                  << db.error().message();
    return false;
  }

  if (!db.set(key, value)) {
    LOG(DB_ERROR) << "Failed to set " << key << " => " << value
                  << " with error: " << db.error().message();
    db.close();
    return false;
  }

  if (!db.close()) {
    LOG(DB_ERROR) << "Failed to close database with error: "
                  << db.error().message();
    return false;
  }

  LOG(DB_VERBOSE) << "Database set " << key << " => " << value;
  return true;
}

bool Database::Get(const String& key, String* value) const {
  using namespace kyotocabinet;

  HashDB db;
  if (!db.open(path_, HashDB::OREADER)) {
    LOG(DB_ERROR) << "Failed to open database with error: "
                  << db.error().message();
    return false;
  }

  if (!db.get(key, value)) {
    LOG(DB_ERROR) << "Failed to get " << key
                  << " with error: " << db.error().message();
    db.close();
    return false;
  }

  if (!db.close()) {
    LOG(DB_ERROR) << "Failed to close database with error: "
                  << db.error().message();
    return false;
  }

  return true;
}

}  // namespace file_cache
}  // namespace dist_clang

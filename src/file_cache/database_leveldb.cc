#include <file_cache/database_leveldb.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/logging.h>
#include <third_party/leveldb/exported/include/leveldb/db.h>

#include <base/using_log.h>

namespace dist_clang {
namespace file_cache {

Database::Database(const String& path, const String& name) {
  using namespace leveldb;

  Options options;
  options.create_if_missing = true;

  String leveldb_path = path + "/leveldb_" + name;
  Status status = DB::Open(options, leveldb_path, &db_);
  if (!status.ok()) {
    LOG(DB_ERROR) << "Failed to open database with error: "
                  << status.ToString();
    db_ = nullptr;
    return;
  }

  LOG(DB_INFO) << "Database is created on path " << leveldb_path;
}

Database::~Database() {
  if (db_) {
    delete db_;
  }
}

bool Database::Set(const String& key, const String& value) {
  using namespace leveldb;

  WriteOptions options;
  options.sync = true;

  if (!db_) {
    return false;
  }

  Status status = db_->Put(options, key, value);
  if (!status.ok()) {
    LOG(DB_ERROR) << "Failed to set " << key << " => " << value
                  << " with error: " << status.ToString();
    return false;
  }

  LOG(DB_VERBOSE) << "Database set " << key << " => " << value;
  return true;
}

bool Database::Get(const String& key, Immutable* value) const {
  using namespace leveldb;

  DCHECK(value);

  ReadOptions options;
  options.fill_cache = true;

  if (!db_) {
    return false;
  }

  String str_value;
  Status status = db_->Get(options, key, &str_value);
  value->assign(Immutable(std::move(str_value)));
  if (!status.ok()) {
    LOG(DB_ERROR) << "Failed to get " << key
                  << " with error: " << status.ToString();
    return false;
  }

  return true;
}

}  // namespace file_cache
}  // namespace dist_clang

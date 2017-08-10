#include <cache/database_leveldb.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/logging.h>
#include <third_party/leveldb/exported/include/leveldb/db.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

LevelDB::LevelDB(const Path& path, const String& name)
    : path_(base::AppendExtension(path / "leveldb_", name.c_str())) {
  using namespace leveldb;

  Options options;
  options.create_if_missing = true;

  Status status = DB::Open(options, path_, &db_);
  CHECK(status.ok()) << "Failed to open database with error: "
                     << status.ToString();

  LOG(DB_INFO) << "Database is created on path " << path_;
}

LevelDB::~LevelDB() {
  DCHECK(db_);
  delete db_;
}

bool LevelDB::Set(const String& key, const Immutable& value) {
  using namespace leveldb;

  WriteOptions options;
  options.sync = true;

  if (!db_) {
    return false;
  }

  Immutable non_const_value = value;
  Status status = db_->Put(
      options, key, Slice(non_const_value.data(), non_const_value.size()));
  if (!status.ok()) {
    LOG(DB_ERROR) << "Failed to set " << key << " => " << value
                  << " with error: " << status.ToString();
    return false;
  }

  LOG(DB_VERBOSE) << "Database set " << key << " => " << value;
  return true;
}

bool LevelDB::Get(const String& key, Immutable* value) const {
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

bool LevelDB::Delete(const String& key) {
  using namespace leveldb;

  WriteOptions options;
  options.sync = true;

  if (!db_) {
    return false;
  }

  Status status = db_->Delete(options, key);
  if (!status.ok()) {
    LOG(DB_ERROR) << "Failed to delete " << key
                  << " with error: " << status.ToString();
    return false;
  }

  LOG(DB_VERBOSE) << "Database delete " << key;
  return true;
}

}  // namespace cache
}  // namespace dist_clang

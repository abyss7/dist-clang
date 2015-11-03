#pragma once

#include <base/aliases.h>
#include <base/attributes.h>
#include <base/file_utils.h>
#include <cache/database.h>

namespace leveldb {
class DB;
}  // namespace leveldb

namespace dist_clang {
namespace cache {

class LevelDB : public Database<Immutable> {
 public:
  LevelDB(const String& path, const String& name);
  ~LevelDB();

  bool Set(const String& key, const Immutable& value) override THREAD_SAFE;
  bool Get(const String& key, Immutable* value) const override THREAD_SAFE;
  bool Delete(const String& key) override THREAD_SAFE;

  inline ui32 GetVersion() const override {
    // No versioning for now.
    return 0;
  }

  inline ui64 SizeOnDisk() const { return base::CalculateDirectorySize(path_); }

 private:
  leveldb::DB* db_ = nullptr;
  const String path_;
};

}  // namespace cache
}  // namespace dist_clang

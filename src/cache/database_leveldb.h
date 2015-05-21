#pragma once

#include <base/aliases.h>
#include <base/attributes.h>
#include <base/file_utils.h>

namespace leveldb {
class DB;
}

namespace dist_clang {
namespace cache {

class Database {
 public:
  Database(const String& path, const String& name);
  ~Database();

  bool Set(const String& key, Immutable value) THREAD_SAFE;
  bool Get(const String& key, Immutable* value) const THREAD_SAFE;
  bool Delete(const String& key) THREAD_SAFE;

  inline ui64 SizeOnDisk() const { return base::CalculateDirectorySize(path_); }

 private:
  leveldb::DB* db_ = nullptr;
  const String path_;
};

}  // namespace cache
}  // namespace dist_clang

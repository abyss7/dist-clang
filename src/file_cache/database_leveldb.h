#pragma once

#include <base/aliases.h>
#include <base/attributes.h>

namespace leveldb {
class DB;
}

namespace dist_clang {
namespace file_cache {

class Database {
 public:
  Database(const String& path, const String& name);
  ~Database();

  bool Set(const String& key, Immutable value) THREAD_SAFE;
  bool Get(const String& key, Immutable* value) const THREAD_SAFE;

 private:
  leveldb::DB* db_ = nullptr;
};

}  // namespace file_cache
}  // namespace dist_clang

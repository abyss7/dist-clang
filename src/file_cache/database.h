#pragma once

#include <base/aliases.h>
#include <base/attributes.h>

#include <third_party/kyoto-cabinet/exported/kchashdb.h>

namespace dist_clang {
namespace file_cache {

class Database {
 public:
  Database(const String& path, const String& name);

  bool Set(const String& key, const String& value) THREAD_UNSAFE;
  bool Get(const String& key, String* value) const THREAD_UNSAFE;

 private:
  const String path_;
};

}  // namespace file_cache
}  // namespace dist_clang

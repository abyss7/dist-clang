#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace cache {

template <class... Values>
class Database {
 public:
  using Value = Tuple<Values...>;

  virtual ~Database() {}

  virtual bool Get(const String& key, Value* values) const = 0;
  virtual bool Set(const String& key, const Value& values) = 0;
  inline virtual bool Exists(const String& key) const {
    Value unused_value;
    return Get(key, &unused_value);
  }
  virtual bool Delete(const String& key) = 0;
};

template <class Value>
class Database<Value> {
 public:
  virtual ~Database() {}

  virtual bool Get(const String& key, Value* values) const = 0;
  virtual bool Set(const String& key, const Value& values) = 0;
  inline virtual bool Exists(const String& key) const {
    Value unused_value;
    return Get(key, &unused_value);
  }
  virtual bool Delete(const String& key) = 0;
};

}  // namespace cache
}  // namespace dist_clang

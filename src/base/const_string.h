#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class ConstString {
 public:
  using Rope = List<ConstString>;

  ConstString() = default;
  explicit ConstString(const char* str);
  ConstString(const char* str, bool take_ownership);
  ConstString(const char str[], size_t size);
  ConstString(UniquePtr<char[]>& str, size_t size);
  explicit ConstString(const String& str);
  explicit ConstString(String&& str);

  // Zero-copy if a rope is a single string, one-copy otherwise.
  explicit ConstString(Rope& rope);
  ConstString(Rope& rope, size_t hint_size);

  ~ConstString();

  inline operator String() const { return string_copy(); }
  inline String string_copy() const { return String(*str_, size_); }

  // Minimal interface for |std::string| compatibility.
  inline bool empty() const { return size_ == 0; }
  inline const char* data() const { return str_.get(); }
  inline const char* c_str() const { return data(); }
  inline size_t size() const { return size_; }
  const char& operator[](size_t index) const;
  ConstString substr(size_t index, size_t length = 0) const;

 private:
  ConstString(const char* str, size_t size, bool take_ownership);

  SharedPtr<const char> str_;
  size_t size_ = 0;

  SharedPtr<String> medium_;
  bool has_ownership_ = true;
};

}  // namespace base
}  // namespace dist_clang

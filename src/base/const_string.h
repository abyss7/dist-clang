#pragma once

#include <base/aliases.h>
#include <base/attributes.h>

namespace dist_clang {

namespace base {
class Literal;
}

inline base::Literal operator"" _l(const char* str, size_t);

namespace base {

class Literal {
 public:
  inline operator const char*() const { return str_; }

 private:
  friend Literal dist_clang::operator"" _l(const char*, size_t);
  friend class ConstString;

  Literal(const char* str) : str_(str) {}

  const char* WEAK_PTR str_ = nullptr;
};

class ConstString {
 public:
  using Rope = List<ConstString>;

  ConstString() = default;                           // 0-copy
  ConstString(Literal str);                          // 0-copy
  ConstString(char str[], size_t size);              // 0-copy
  ConstString(UniquePtr<char[]>& str, size_t size);  // 0-copy
  ConstString(String&& str);                         // 0-copy
  ConstString(Rope&& rope);                          // 0-copy
  ConstString(Rope&& rope, size_t hint_size);        // 0-copy
  ConstString(const Rope& rope);                     // 0-copy
  ConstString(const Rope& rope, size_t hint_size);   // 0-copy

  explicit ConstString(const String& str);  // 1-copy

  inline operator String() const { return string_copy(); }  // 1-copy
  String string_copy() const;                               // 1-copy

  // Minimal interface for |std::string| compatibility.
  ConstString substr(size_t index, size_t length = 0) const;  // 0-copy
  const char* data() const;                                   // 0,1-copy
  const char* c_str() const;                                  // 0,1-copy
  inline size_t size() const { return size_; }                // 0-copy
  inline bool empty() const { return size_ == 0; }            // 0-copy

  const char& operator[](size_t index) const;             // 0-copy
  ConstString operator+(const ConstString& other) const;  // 0-copy

 private:
  ConstString(const char* str, size_t size, bool null_end);  // 0-copy

  void CollapseRope() const;
  void NullTerminate() const;

  mutable SharedPtr<String> medium_;
  mutable SharedPtr<const char> str_;
  mutable Rope rope_;
  mutable size_t size_ = 0;
  mutable bool null_end_ = false;
};

}  // namespace base

base::Literal operator"" _l(const char* str, size_t) {
  return base::Literal(str);
}

}  // namespace dist_clang

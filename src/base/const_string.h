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
  inline size_t size() const { return strlen(str_); }
  inline bool operator==(const Literal& other) const {
    return strcmp(str_, other.str_) == 0;
  }
  inline bool operator!=(const Literal& other) const {
    return !this->operator==(other);
  }
  inline bool operator==(const ConstString& other) const;
  inline bool operator!=(const ConstString& other) const;
  inline bool operator==(const String& other) const { return other == str_; }

  static const Literal empty;

 private:
  friend Literal dist_clang::operator"" _l(const char*, size_t);
  friend class ConstString;
  friend Literal GetEnv(Literal, Literal);

  Literal(const char* str) : str_(str) {}

  const char* WEAK_PTR str_ = nullptr;
};

class ConstString {
 public:
  using Rope = List<ConstString>;

  ConstString() = default;                           // 0-copy
  ConstString(Literal str);                          // 0-copy
  ConstString(char str[]);                           // 0-copy
  ConstString(UniquePtr<char[]>& str);               // 0-copy
  ConstString(char str[], size_t size);              // 0-copy
  ConstString(UniquePtr<char[]>& str, size_t size);  // 0-copy
  ConstString(String&& str);                         // 0-copy
  ConstString(String* str);                          // 0-copy
  ConstString(Rope&& rope);                          // 0-copy
  ConstString(Rope&& rope, size_t hint_size);        // 0-copy
  ConstString(const Rope& rope);                     // 0-copy
  ConstString(const Rope& rope, size_t hint_size);   // 0-copy

  explicit ConstString(const String& str);           // 1-copy
  static ConstString WrapString(const String& str);  // 0-copy

  inline operator String() const { return string_copy(); }  // 1-copy
  String string_copy() const;                               // 1-copy

  // Minimal interface for |std::string| compatibility.
  inline void assign(const ConstString& other) { this->operator=(other); }
  ConstString substr(size_t index, size_t length = 0) const;  // 0-copy
  const char* data() const;                                   // 0,1-copy
  const char* c_str() const;                                  // 0,1-copy
  inline size_t size() const { return size_; }                // 0-copy
  inline bool empty() const { return size_ == 0; }            // 0-copy
  bool operator==(const ConstString& other) const;            // 0-copy
  inline bool operator!=(const ConstString& other) const {    // 0-copy
    return !this->operator==(other);
  }

  size_t find(const char* str) const;  // 1-copy

  const char& operator[](size_t index) const;             // 0-copy
  ConstString operator+(const ConstString& other) const;  // 0-copy

 private:
  ConstString(const char* WEAK_PTR str, size_t size, bool null_end);  // 0-copy

  void CollapseRope() const;
  void NullTerminate() const;

  mutable SharedPtr<String> medium_;
  mutable SharedPtr<const char> str_;
  mutable Rope rope_;
  mutable size_t size_ = 0;
  mutable bool null_end_ = false;
};

bool Literal::operator==(const ConstString& other) const {
  return other == *this;
}

bool Literal::operator!=(const ConstString& other) const {
  return other != *this;
}

}  // namespace base

base::Literal operator"" _l(const char* str, size_t) {
  return base::Literal(str);
}

}  // namespace dist_clang

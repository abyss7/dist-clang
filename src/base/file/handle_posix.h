#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class Handle {
 public:
  using NativeType = int;

  Handle() = default;
  explicit Handle(NativeType fd);
  explicit Handle(Handle&& other);
  Handle& operator=(Handle&& other);

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle& other) = delete;

  ~Handle();

  inline bool operator==(const Handle& other) const { return fd_ == other.fd_; }

  inline bool IsValid() const { return fd_ != -1; }
  bool Duplicate(Handle&& other, String* error = nullptr);
  void Close();

  bool CloseOnExec(String* error = nullptr);

  inline NativeType native() const { return fd_; }

 protected:
  bool IsPassive() const;

 private:
  NativeType fd_ = -1;
};

}  // namespace base
}  // namespace dist_clang

namespace std {

template <>
struct hash<dist_clang::base::Handle> {
 public:
  size_t operator()(const dist_clang::base::Handle& value) const {
    return std::hash<dist_clang::base::Handle::NativeType>()(value.native());
  }
};

}  // namespace std

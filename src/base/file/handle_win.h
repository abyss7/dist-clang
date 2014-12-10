#pragma once

#include <base/aliases.h>

#include <windows.h>

namespace dist_clang {
namespace base {

class Handle {
 public:
  using NativeType = HANDLE;

  Handle() = default;
  explicit Handle(NativeType fd);
  Handle(Handle&& other);
  Handle& operator=(Handle&& other);

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle& other) = delete;

  ~Handle();

  inline bool operator==(const Handle& other) const { return fd_ == other.fd_; }

  inline bool IsValid() const { return fd_ != INVALID_HANDLE_VALUE; }
  bool Duplicate(Handle&& other, String* error = nullptr);
  void Close();

  bool CloseOnExec(String* error = nullptr);

  inline NativeType native() const { return fd_; }

 private:
  NativeType fd_ = INVALID_HANDLE_VALUE;
};

}  // namespace base
}  // namespace dist_clang

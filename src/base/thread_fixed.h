#pragma once

#include <base/assert.h>

#include STL(thread)

namespace dist_clang {
namespace base {

class ThreadFixed {
 public:
  ThreadFixed() = default;
  ThreadFixed(const ThreadFixed&) : id_(std::this_thread::get_id()) {}
  inline ThreadFixed& operator=(const ThreadFixed&) { return *this; }

  inline void CheckThread() const { DCHECK(id_ == std::this_thread::get_id()); }

 private:
  const std::thread::id id_ = std::this_thread::get_id();
};

}  // namespace base
}  // namespace dist_clang

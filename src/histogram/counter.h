#pragma once

#include <base/aliases.h>

#include <chrono>
#include <string>

namespace dist_clang {
namespace histogram {

class Counter {
 public:
  Counter(const char* label);
  Counter(const char* label, ui64 value);
  ~Counter();

 private:
  const ui64 id_;
  const ui64 value_ = 0u;
};

}  // namespace base
}  // namespace dist_clang

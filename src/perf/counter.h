#pragma once

#include <base/aliases.h>
#include <base/attributes.h>

namespace dist_clang {
namespace perf {

class Reporter {
 public:
  virtual ~Reporter() {}

  virtual void Report(const TimePoint& start, const TimePoint& end) const = 0;
};

template <class T>
class Counter final {
 public:
  template <class... Args>
  Counter(Args... args)
      : reporter_(new T(args...)), id_(next_id()) {}
  ~Counter() { reporter_->Report(start_, Clock::now()); }

  Counter(const Counter&) = delete;

  inline ui64 Id() const { return id_; }

 private:
  static ui64 next_id() {
    static std::atomic<ui64> next_id(1);
    return next_id++;
  }

  UniquePtr<Reporter> reporter_;
  const TimePoint start_ = Clock::now();
  const ui64 id_;
};

}  // namespace perf
}  // namespace dist_clang

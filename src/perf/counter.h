#pragma once

#include <base/types.h>

namespace dist_clang {
namespace perf {

class Reporter {
 public:
  virtual ~Reporter() {}

  virtual void Report(const TimePoint& start, const TimePoint& end) const = 0;
};

template <class T, bool ReportByDefault = true>
class Counter final {
 public:
  template <class... Args>
  Counter(Args... args)
      : reporter_(new T(args...)), id_(next_id()) {}
  ~Counter() {
    if (report_on_destroy_ && reporter_) {
      Report();
    }
  }

  Counter(const Counter&) = delete;

  inline ui64 Id() const { return id_; }
  inline void ReportOnDestroy(bool report) { report_on_destroy_ = report; }
  inline void Report() {
    DCHECK(reporter_);
    reporter_->Report(start_, Clock::now());
    reporter_.reset();
  }

 private:
  static ui64 next_id() {
    static Atomic<ui64> next_id(1);
    return next_id++;
  }

  UniquePtr<Reporter> reporter_;
  const TimePoint start_ = Clock::now();
  const ui64 id_;
  bool report_on_destroy_ = ReportByDefault;
};

}  // namespace perf
}  // namespace dist_clang

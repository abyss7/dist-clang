#pragma once

#include <base/aliases.h>

#include <third_party/libcxx/exported/include/chrono>

namespace dist_clang {
namespace histogram {

class Collector {
 public:
  ui64 Start(const char* label);
  void Report(ui64 id, ui64 value);
  void Stop(ui64 id);

 private:
  using Clock = std::chrono::steady_clock;
  struct Interval {
    Clock::time_point start, stop;
    ui64 value;
  };

  std::atomic<ui64> next_id_ = {0u};
  HashMap<ui64 /* id */, const char* /* label */> labels_;
  HashMap<ui64 /* id */, Interval> intervals_;
};

}  // namespace histogram
}  // namespace dist_clang

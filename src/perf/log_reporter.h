#pragma once

#include <base/const_string.h>
#include <perf/counter.h>

namespace dist_clang {
namespace perf {

class LogReporter : public Reporter {
 public:
  enum Type {
    HUMAN = 1,
    TEAMCITY = 2,
  };

  explicit LogReporter(Literal label, Type type = HUMAN);

 private:
  void Report(const TimePoint& start, const TimePoint& end) const override;

  const Literal label_;
  const Type type_;
};

}  // namespace perf
}  // namespace dist_clang

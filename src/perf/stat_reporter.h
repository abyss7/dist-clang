#pragma once

#include <perf/counter.h>
#include <perf/stat_service.h>

namespace dist_clang {
namespace perf {

class StatReporter : public Reporter {
 public:
  explicit StatReporter(proto::Metric::Name name);

 private:
  void Report(const TimePoint& start, const TimePoint& end) const override;

  const proto::Metric::Name name_;
};

}  // namespace perf
}  // namespace dist_clang

#include <perf/stat_reporter.h>

namespace dist_clang {
namespace perf {

StatReporter::StatReporter(proto::Metric::Name name) : name_(name) {}

void StatReporter::Report(const TimePoint& start, const TimePoint& end) const {
  // TODO: implement different types of metrics. Right now just report the time
  //       difference in milliseconds.
  using namespace std::chrono;

  base::Singleton<StatService>::Get().Add(
      name_, duration_cast<milliseconds>(end - start).count());
}

}  // namespace perf
}  // namespace dist_clang

#include <perf/log_reporter.h>

#include <base/logging.h>

#include <base/using_log.h>

namespace dist_clang {
namespace perf {

LogReporter::LogReporter(Literal label, Type type)
    : label_(label), type_(type) {
}

void LogReporter::Report(const TimePoint& start, const TimePoint& end) const {
  using namespace std::chrono;

  switch (type_) {
    case HUMAN: {
      LOG(TRACE) << label_ << " took "
                 << duration_cast<milliseconds>(end - start).count() << " ms";
    } break;

    case TEAMCITY: {
      LOG(TRACE) << "##teamcity[buildStatisticValue key='" << label_
                 << "' value='"
                 << duration_cast<milliseconds>(end - start).count() << "']";
    } break;
  }
}

}  // namespace perf
}  // namespace dist_clang

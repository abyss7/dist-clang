#include <perf/stat_service.h>

#include <base/assert.h>

namespace dist_clang {

DEFINE_SINGLETON(perf::StatService)

namespace perf {

StatService::StatService() {
  for (auto& value : values_) {
    value.store(0u);
  }
}

void StatService::Add(proto::Metric::Name name, ui64 value) {
  values_[name].fetch_add(value);
}

void StatService::Dump(proto::Metric& report) {
  CHECK(report.has_name());

  auto name = report.name();
  report.set_value(values_[name].exchange(0u));
}

}  // namespace perf
}  // namespace dist_clang

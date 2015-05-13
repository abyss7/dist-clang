#include <perf/stat_service.h>

#include <base/assert.h>

namespace dist_clang {
namespace perf {

StatService::StatService() {
  for (auto& ptr : values_) {
    ptr.reset(new Atomic<ui64>(0u));
  }
}

void StatService::Add(proto::Metric::Name name, ui64 value) {
  values_[name]->fetch_add(value);
}

void StatService::Dump(proto::Metric& report) {
  CHECK(report.has_name());

  auto name = report.name();
  auto old_value = values_[name];
  values_[name].reset(new Atomic<ui64>(0u));

  report.set_value(*old_value);
}

}  // namespace perf
}  // namespace dist_clang

#include <perf/stat_service.h>

#include <base/assert.h>

namespace dist_clang {

DEFINE_SINGLETON(perf::StatService)

namespace perf {

StatService::StatService() = default;

void StatService::Add(proto::Metric::Name name, ui64 value) {
  values_[name].fetch_add(value);
}

void StatService::Set(proto::Metric::Name name, ui64 value) {
  values_[name].store(value);
}

void StatService::DumpAll(proto::Report* report) const {
  CHECK(report);

  for (int name = proto::Metric::Name_MIN; name <= proto::Metric::Name_MAX;
       ++name) {
    if (!proto::Metric::Name_IsValid(name)) {
      continue;
    }
    auto* metric = report->add_metric();
    metric->set_name(static_cast<proto::Metric::Name>(name));
    metric->set_value(values_[name].load());
  }
}

void StatService::Dump(proto::Metric& report) {
  CHECK(report.has_name());

  report.set_value(values_[report.name()].exchange(0u));
}

}  // namespace perf
}  // namespace dist_clang

#pragma once

#include <base/singleton.h>
#include <perf/stat.pb.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#define STAT(metric_name, ...)                   \
  base::Singleton<perf::StatService>::Get().Add( \
      perf::proto::Metric::metric_name, ##__VA_ARGS__)
#pragma clang diagnostic pop

namespace dist_clang {
namespace perf {

class StatService {
 public:
  StatService();

  void Add(proto::Metric::Name name, ui64 value = 1);
  void Set(proto::Metric::Name name, ui64 value);
  void Dump(proto::Metric& report);

  // Adds all metrics to |report| without resetting them.
  void DumpAll(proto::Report* report) const;

 private:
  // The counters are never replaced: the collector takes them atomically.
  Array<Atomic<ui64>, proto::Metric::Name_ARRAYSIZE> values_ = {};
};

}  // namespace perf

DECLARE_SINGLETON(perf::StatService)

}  // namespace dist_clang

#pragma once

#include <base/singleton.h>
#include <perf/stat.pb.h>

#define STAT(metric_name, ...)                   \
  base::Singleton<perf::StatService>::Get().Add( \
      perf::proto::Metric::metric_name, ##__VA_ARGS__)

namespace dist_clang {
namespace perf {

class StatService {
 public:
  StatService();

  void Add(proto::Metric::Name name, ui64 value = 1);
  void Dump(proto::Metric& report);

 private:
  Array<Atomic<ui64>, proto::Metric::Name_ARRAYSIZE> values_;
};

}  // namespace perf

DECLARE_SINGLETON(perf::StatService)

}  // namespace dist_clang

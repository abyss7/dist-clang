#include <perf/stat_service.h>

#include <gtest/gtest.h>

#include STL(thread)

namespace dist_clang {
namespace perf {

TEST(StatServiceTest, AddAndDump) {
  proto::Metric metric;

  // Clear state of a singleton.
  metric.set_name(proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<StatService>::Get().Dump(metric);

  metric.set_name(proto::Metric::SIMPLE_CACHE_HIT);
  base::Singleton<StatService>::Get().Dump(metric);

  base::Singleton<StatService>::Get().Add(proto::Metric::DIRECT_CACHE_HIT);

  metric.set_name(proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  metric.set_name(proto::Metric::SIMPLE_CACHE_HIT);
  base::Singleton<StatService>::Get().Dump(metric);
  EXPECT_EQ(0u, metric.value());

  metric.set_name(proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<StatService>::Get().Dump(metric);
  EXPECT_EQ(0u, metric.value());
}

TEST(StatServiceTest, DumpWhileAdded) {
  // The workers count, while the collector takes the values: nothing is lost.
  auto& service = base::Singleton<StatService>::Get();
  proto::Metric metric;
  metric.set_name(proto::Metric::LOCAL_TASK_DONE);
  service.Dump(metric);

  Vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&service] {
      for (int j = 0; j < 10000; ++j) {
        service.Add(proto::Metric::LOCAL_TASK_DONE);
      }
    });
  }
  ui64 total = 0;
  for (int i = 0; i < 1000; ++i) {
    service.Dump(metric);
    total += metric.value();
  }
  for (auto& worker : workers) {
    worker.join();
  }
  service.Dump(metric);
  total += metric.value();
  EXPECT_EQ(40000u, total);
}

}  // namespace perf
}  // namespace dist_clang

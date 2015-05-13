#include <perf/stat_service.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

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

}  // namespace perf
}  // namespace dist_clang

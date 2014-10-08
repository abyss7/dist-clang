#include <daemon/absorber.h>

#include <daemon/configuration.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace daemon {

TEST(AbsorberTest, ConfigWithoutAbsorber) {
  proto::Configuration conf;

  EXPECT_ANY_THROW({ delete new Absorber(conf); });
}

}  // namespace daemon
}  // namespace dist_clang

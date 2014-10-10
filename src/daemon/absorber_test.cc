#include <daemon/absorber.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace daemon {

TEST(AbsorberConfigurationTest, NoAbsorberSection) {
  ASSERT_ANY_THROW((Absorber((proto::Configuration()))));
}

TEST(AbsorberConfigurationTest, NoLocalHost) {
  proto::Configuration conf;
  conf.mutable_absorber();

  ASSERT_ANY_THROW(Absorber absorber(conf));
}

}  // namespace daemon
}  // namespace dist_clang

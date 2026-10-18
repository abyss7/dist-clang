#include <client/driver_name.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace client {

TEST(DriverNameTest, DriverMode) {
  // The mode of the real compiler doesn't matter: the name of the symlink
  // sets it.
  EXPECT_EQ(List<String>{"--driver-mode=g++"}, DriverNameArgs("clang++"));
  EXPECT_EQ(List<String>{"--driver-mode=gcc"}, DriverNameArgs("clang"));
  EXPECT_EQ(List<String>{"--driver-mode=g++"}, DriverNameArgs("c++"));
  EXPECT_EQ(List<String>{"--driver-mode=gcc"}, DriverNameArgs("cc"));
  EXPECT_EQ(List<String>{"--driver-mode=g++"}, DriverNameArgs("clang++-18"));
}

}  // namespace client
}  // namespace dist_clang

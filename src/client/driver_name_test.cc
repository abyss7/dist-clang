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

TEST(DriverNameTest, Target) {
  // The prefix of the name is the target, like for the compiler itself.
  EXPECT_EQ((List<String>{"--driver-mode=g++", "--target=aarch64-linux-gnu"}),
            DriverNameArgs("aarch64-linux-gnu-clang++"));
  EXPECT_EQ((List<String>{"--driver-mode=gcc", "--target=arm-none-eabi"}),
            DriverNameArgs("arm-none-eabi-clang-18"));
  // Not a triple.
  EXPECT_EQ(List<String>{"--driver-mode=gcc"}, DriverNameArgs("my-clang"));
}

}  // namespace client
}  // namespace dist_clang

#include <client/driver_name.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

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

TEST(DriverNameTest, RemoveOutputsBeforeLocalRun) {
  const base::TemporaryDir temp_dir;
  const auto blob = temp_dir.path() / "blob";
  const auto object = temp_dir.path() / "a.o";
  const auto dwo = temp_dir.path() / "a.dwo";
  ASSERT_TRUE(base::File::Write(blob, "cached"_l));
  // The outputs are the links of the files of the cache: the compiler may
  // write the split DWARF in place - into the cache.
  ASSERT_TRUE(base::File::Link(blob, object));
  ASSERT_TRUE(base::File::Link(blob, dwo));

  RemoveOutputs({"clang++", "-c", "a.cc", "-o", object.string(), "-g",
                 "-gsplit-dwarf"});
  EXPECT_FALSE(base::File::Exists(object));
  EXPECT_FALSE(base::File::Exists(dwo));
  EXPECT_TRUE(base::File::Exists(blob));
}

}  // namespace client
}  // namespace dist_clang

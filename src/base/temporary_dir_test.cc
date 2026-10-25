#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <stdlib.h>

namespace dist_clang {
namespace base {

namespace {

// Sets the environment variable for the scope, restores it then.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = getenv(name)) {
      old_ = old;
      had_ = true;
    }
    if (value) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnv() {
    if (had_) {
      setenv(name_, old_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

 private:
  const char* name_;
  String old_;
  bool had_ = false;
};

}  // namespace

TEST(TemporaryDirTest, TakesTheDirectoryFromTMPDIR) {
  // E.g. to run the tests on another file system.
  const TemporaryDir parent;
  ASSERT_FALSE(parent.path().empty());
  const ScopedEnv env("TMPDIR", parent.path().c_str());

  const TemporaryDir temp_dir;
  EXPECT_EQ(parent.path(), temp_dir.path().parent_path());
  EXPECT_TRUE(std::filesystem::is_directory(temp_dir.path()));
}

TEST(TemporaryDirTest, TakesTmpWithoutTMPDIR) {
  for (const char* value : {static_cast<const char*>(nullptr), ""}) {
    const ScopedEnv env("TMPDIR", value);
    const TemporaryDir temp_dir;
    EXPECT_EQ(std::filesystem::canonical("/tmp"),
              temp_dir.path().parent_path());
  }
}

TEST(TemporaryDirTest, ResolvesSymlinks) {
  // The tests compare the paths, that the code resolves - e.g. TMPDIR is
  // "/var/tmp" -> "/place/vartmp", or "/var/folders/..." on Mac.
  const TemporaryDir parent;
  ASSERT_FALSE(parent.path().empty());
  const Path real = parent.path() / "real";
  const Path link = parent.path() / "link";
  ASSERT_TRUE(std::filesystem::create_directory(real));
  std::filesystem::create_directory_symlink(real, link);
  const ScopedEnv env("TMPDIR", link.c_str());

  const TemporaryDir temp_dir;
  EXPECT_EQ(std::filesystem::canonical(real), temp_dir.path().parent_path());
}

}  // namespace base
}  // namespace dist_clang

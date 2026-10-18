#include <client/autostart.h>

#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

namespace dist_clang {
namespace client {

TEST(AutostartTest, BackoffAfterFailedStart) {
  const base::TemporaryDir temp_dir;
  const int fd =
      open((temp_dir.path() / "lock").c_str(), O_RDWR | O_CREAT, 0600);
  ASSERT_NE(-1, fd);

  // The first start is allowed.
  bool warn = true;
  EXPECT_TRUE(AllowStart(fd, 1000, &warn));
  EXPECT_FALSE(warn);

  // The lock is free soon again - the daemon has failed to start: don't start
  // it for every compilation, and warn once.
  EXPECT_FALSE(AllowStart(fd, 1001, &warn));
  EXPECT_TRUE(warn);
  EXPECT_FALSE(AllowStart(fd, 1002, &warn));
  EXPECT_FALSE(warn);

  // A later try.
  EXPECT_TRUE(AllowStart(fd, 1000 + kAutostartBackoff, &warn));
  EXPECT_FALSE(warn);
  close(fd);
}

}  // namespace client
}  // namespace dist_clang

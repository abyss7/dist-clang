#include <gtest/gtest.h>

int main(int argc, char* argv[]) {
  // Ignore SIGPIPE to prevent application crashes.
  signal(SIGPIPE, SIG_IGN);

  ::testing::InitGoogleTest(&argc, argv);
  ::testing::FLAGS_gtest_death_test_style = "fast";
  return RUN_ALL_TESTS();
}

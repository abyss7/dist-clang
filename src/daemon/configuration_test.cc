#include <daemon/configuration.h>

#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <stdlib.h>

namespace dist_clang {
namespace daemon {

TEST(DaemonConfigurationTest, SocketOfAutostart) {
  // The configuration of the user is not read.
  const base::TemporaryDir temp_dir;
  setenv("XDG_CONFIG_HOME", temp_dir.path().c_str(), 1);

  // The client passes the socket, that it has locked - the daemon must listen
  // there.
  const char* args[] = {"dist-clangd", "--autostart", "--lock-fd=3",
                        "--socket=/run/test/x.sock"};
  Configuration configuration(4, const_cast<char**>(args));
  EXPECT_EQ("/run/test/x.sock", configuration.config().emitter().socket_path());
  EXPECT_EQ(3, configuration.lock_fd());

  unsetenv("XDG_CONFIG_HOME");
}

}  // namespace daemon
}  // namespace dist_clang

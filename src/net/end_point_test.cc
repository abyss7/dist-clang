#include <net/end_point.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace net {

TEST(EndPointTest, UnixSocketPathLimit) {
  // The path is cut by the system without the check - e.g. the long
  // $XDG_RUNTIME_DIR: then the daemon and its clients don't find each other.
  const String short_path = "/run/user/1000/dist-clang.socket";
  EXPECT_EQ(String(), EndPoint::UnixSocketError(short_path));
  EXPECT_TRUE(EndPoint::UnixSocket(short_path));

  const String long_path = "/tmp/" + String(200, 'a') + "/dist-clang.socket";
  const String error = EndPoint::UnixSocketError(long_path);
  EXPECT_NE(String::npos, error.find(long_path)) << error;
  EXPECT_NE(String::npos, error.find("DC_SOCKET_PATH")) << error;
  EXPECT_FALSE(EndPoint::UnixSocket(long_path));
}

}  // namespace net
}  // namespace dist_clang

#include <net/connection.h>

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

TEST(ConnectionTest, SocketPeerUid) {
  int sockets[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  ui32 uid = getuid() + 1;
  EXPECT_TRUE(Connection::SocketPeerUid(sockets[0], &uid));
  EXPECT_EQ(getuid(), uid);
  close(sockets[0]);
  close(sockets[1]);

  EXPECT_FALSE(Connection::SocketPeerUid(-1, &uid));
}

}  // namespace net
}  // namespace dist_clang

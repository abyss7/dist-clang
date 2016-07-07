#include <net/end_point.h>

#include <net/passive.h>
#include <net/socket.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace net {

TEST(EndPointTest, Unix) {
  EndPointPtr ep = EndPoint::UnixSocket("/tmp/socket");
  ASSERT_EQ("/tmp/socket", ep->Address());
  ASSERT_EQ(0, ep->Port());
}

TEST(EndPointTest, Inet4) {
  EndPointPtr ep = EndPoint::TcpHost("127.0.0.1", 12345, false);
  ASSERT_EQ("127.0.0.1", ep->Address());
  ASSERT_EQ(12345, ep->Port());
}

TEST(EndPointTest, Inet6) {
  EndPointPtr ep = EndPoint::TcpHost("::1", 12346, true);
  ASSERT_EQ("::1", ep->Address());
  ASSERT_EQ(12346, ep->Port());
}

TEST(EndPointTest, FromPassive) {
  EndPointPtr expected = EndPoint::LocalHost("ya.ru", 12345, false);
  Socket socket(expected);
  socket.MakeBlocking(false);
  socket.Bind(expected);
  Passive listener(std::move(socket));
  EndPointPtr actual = EndPoint::FromPassive(listener);
  ASSERT_EQ("127.0.0.1", actual->Address());
  ASSERT_GT(1024, expected->Port());
}

}  // namespace net
}  // namespace dist_clang

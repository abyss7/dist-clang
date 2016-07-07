#include <net/network_service_impl.h>

#include <base/aliases.h>
#include <base/thread.h>
#include <net/end_point.h>
#include <net/socket.h>
#include <net/passive.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace net {

TEST(NetworkServiceTest, ConnectTimedOut) {
  auto service = net::NetworkService::Create(0, 10, 10, 1);
  EndPointPtr listen = EndPoint::LocalHost("localhost", 0, false);
  Socket socket(listen);
  socket.MakeBlocking(false);
  ASSERT_TRUE(socket.IsValid());
  ASSERT_TRUE(socket.Bind(listen));
  Passive listener(std::move(socket));
  ASSERT_TRUE(listener.IsValid());

  auto start = Clock::now();
  ASSERT_FALSE(!!service->Connect(EndPoint::FromPassive(listener), nullptr));
  ASSERT_GT(std::chrono::seconds(2), Clock::now() - start);
}

TEST(NetworkServiceTest, ConnectSucceeded) {
  auto service = net::NetworkService::Create(0, 0, 0, 1);
  EndPointPtr listen = EndPoint::LocalHost("localhost", 0, false);
  Socket socket(listen);
  socket.MakeBlocking(false);
  socket.Bind(listen);
  Passive listener(std::move(socket));

  Thread accepter("Accepter"_l, [&listener] {
    Socket client = listener.Accept();
  });

  auto start = Clock::now();
  ASSERT_TRUE(!!service->Connect(EndPoint::FromPassive(listener), nullptr));
  ASSERT_GT(std::chrono::seconds(2), Clock::now() - start);

  accepter.join();
}

}  // namespace net
}  // namespace dist_clang

#include <net/network_service_impl.h>

#include <base/aliases.h>
#include <base/thread.h>
#include <net/end_point.h>
#include <net/socket.h>
#include <net/passive.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace net {

class NetworkServiceTest : public ::testing::Test {
 public:
  virtual void SetUp() override {
    NetworkService::SetFactory<NetworkService::DefaultFactory>();
  }
};

TEST_F(NetworkServiceTest, ConnectTimedOut) {
  auto service = net::NetworkService::Create(0, 0, 0, 1);
  EndPointPtr unreachable = EndPoint::TcpHost("example.com", 12345, false);

  auto start = Clock::now();
  ASSERT_FALSE(!!service->Connect(unreachable, nullptr));
  auto duration = Clock::now() - start;
  ASSERT_LT(std::chrono::seconds(1), duration);
  ASSERT_GT(std::chrono::seconds(2), duration);
}

TEST_F(NetworkServiceTest, ConnectSucceeded) {
  auto service = net::NetworkService::Create(0, 0, 0, 1);
  EndPointPtr listen = EndPoint::TcpHost("localhost", 0, false);

  Socket socket(listen);
  ASSERT_TRUE(socket.IsValid());
  socket.MakeBlocking(false);
  ASSERT_TRUE(socket.Bind(listen));

  Passive listener(std::move(socket));
  ASSERT_TRUE(listener.IsValid());

  auto actual = EndPoint::FromPassive(listener);
  ASSERT_TRUE(!!actual);

  auto start = Clock::now();
  ASSERT_TRUE(!!service->Connect(actual, nullptr));
  ASSERT_GT(std::chrono::seconds(2), Clock::now() - start);
}

}  // namespace net
}  // namespace dist_clang

#include <net/network_service_impl.h>

#include <base/temporary_dir.h>
#include <net/connection.h>
#include <net/end_point.h>

#include <gtest/gtest.h>

#include STL(thread)

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

TEST(NetworkServiceImplTest, ConnectToBusySocket) {
  const base::TemporaryDir temp_dir;
  const String path = (temp_dir.path() / "socket").string();

  // The daemon is busy - e.g. it's starting: the backlog of its socket is
  // full.
  const int server = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_NE(-1, server);
  struct sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  ASSERT_EQ(0, bind(server, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)));
  ASSERT_EQ(0, listen(server, 1));
  ASSERT_EQ(0, fcntl(server, F_SETFL, O_NONBLOCK));
  List<int> clients;
  while (true) {
    const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(-1, client);
    clients.push_back(client);
    if (connect(client, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == -1) {
      ASSERT_EQ(EAGAIN, errno);
      break;
    }
  }

  // The daemon accepts the connections a bit later - for a while.
  std::thread accepter([server] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < deadline) {
      const int connection = accept4(server, nullptr, nullptr, SOCK_NONBLOCK);
      if (connection != -1) {
        close(connection);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  });

  NetworkService::SetFactory<NetworkService::DefaultFactory>();
  auto service = NetworkService::Create(5, 5, 5, 1);
  String error;
  EXPECT_TRUE(service->Connect(EndPoint::UnixSocket(path), &error)) << error;

  accepter.join();
  for (int client : clients) {
    close(client);
  }
  close(server);
}

}  // namespace net
}  // namespace dist_clang

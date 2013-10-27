#include "net/epoll_event_loop.h"

#include <gtest/gtest.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>

namespace dist_clang {
namespace net {

// First of all, we need to be sure, that our basic suggestions about the
// epoll_ctl and epoll_wait mechanism are correct.

TEST(EpollTest, PersistentEPOLLHUP) {
  const int a = 1;
  int test_pipe[2];
  struct epoll_event event;
  int epoll_fd = epoll_create1(0);

  ASSERT_NE(-1, pipe(test_pipe));
  ASSERT_NE(-1, epoll_fd);
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = test_pipe[0];
  ASSERT_NE(-1, epoll_ctl(epoll_fd, EPOLL_CTL_ADD, test_pipe[0], &event));

  // FIXME: g++ complains about |a|, if it's not passed by value.
  auto lambda = [&, a]() {
    ASSERT_EQ(static_cast<int>(sizeof(a)), write(test_pipe[1], &a, sizeof(a)));
    ASSERT_NE(-1, close(test_pipe[1]));
  };
  std::thread thread(lambda);
  thread.join();

  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(1, epoll_wait(epoll_fd, &event, 1, -1));
    EXPECT_EQ(test_pipe[0], event.data.fd);
    EXPECT_TRUE(event.events & EPOLLIN);
    EXPECT_TRUE(event.events & EPOLLHUP);
    int data = 0;
    ASSERT_NE(-1, ioctl(test_pipe[0], FIONREAD, &data));
    EXPECT_EQ(static_cast<int>(sizeof(a)), data);
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.fd = test_pipe[0];
    ASSERT_NE(-1, epoll_ctl(epoll_fd, EPOLL_CTL_MOD, test_pipe[0], &event));
  }

  close(test_pipe[0]);
  close(epoll_fd);
}

}  // namespace net
}  // namespace dist_clang

#include <net/kqueue_event_loop.h>

#include <third_party/gtest/public/gtest/gtest.h>

#include <sys/event.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

// First of all, we need to be sure, that our basic suggestions about the
// kqueue and kevent mechanism are correct.

TEST(KqueueTest, Add) {
  const int a = 1;
  int test_pipe[2];
  struct kevent event;
  int kq_fd = kqueue();

  ASSERT_NE(-1, pipe(test_pipe));
  ASSERT_NE(-1, kq_fd);
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  auto lambda = [&]() {
    ASSERT_EQ(static_cast<int>(sizeof(a)), write(test_pipe[1], &a, sizeof(a)));
  };
  std::thread thread(lambda);

  ASSERT_EQ(1, kevent(kq_fd, nullptr, 0, &event, 1, nullptr));
  EXPECT_EQ(test_pipe[0], static_cast<int>(event.ident));
  EXPECT_EQ(EVFILT_READ, event.filter);
  EXPECT_EQ(static_cast<int>(sizeof(a)), event.data);

  // Double-EV_ADD should re-enable event.
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  ASSERT_EQ(1, kevent(kq_fd, nullptr, 0, &event, 1, nullptr));
  EXPECT_EQ(test_pipe[0], static_cast<int>(event.ident));
  EXPECT_EQ(EVFILT_READ, event.filter);
  EXPECT_EQ(static_cast<int>(sizeof(a)), event.data);

  thread.join();
  close(test_pipe[0]);
  close(test_pipe[1]);
}

TEST(KqueueTest, OneShot) {
  const int a = 1;
  int test_pipe[2];
  struct kevent event;
  int kq_fd = kqueue();

  ASSERT_NE(-1, pipe(test_pipe));
  ASSERT_NE(-1, kq_fd);
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  auto lambda = [&]() {
    ASSERT_EQ(static_cast<int>(sizeof(a)), write(test_pipe[1], &a, sizeof(a)));
  };
  std::thread thread(lambda);

  ASSERT_EQ(1, kevent(kq_fd, nullptr, 0, &event, 1, nullptr));
  EXPECT_EQ(test_pipe[0], static_cast<int>(event.ident));
  EXPECT_EQ(EVFILT_READ, event.filter);
  EXPECT_EQ(static_cast<int>(sizeof(a)), event.data);

  // Triggering an one-shot event should disable it.
  struct timespec time_spec = {1, 0};
  ASSERT_EQ(0, kevent(kq_fd, nullptr, 0, &event, 1, &time_spec));

  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ENABLE | EV_ONESHOT, 0, 0, 0);
  ASSERT_EQ(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));
  EXPECT_EQ(ENOENT, errno);

  thread.join();
  close(test_pipe[0]);
  close(test_pipe[1]);
}

TEST(KqueueTest, Disable) {
  const int a = 1;
  int test_pipe[2];
  struct kevent event;
  int kq_fd = kqueue();

  ASSERT_NE(-1, pipe(test_pipe));
  ASSERT_NE(-1, kq_fd);

  // EV_ADD should imply EV_ENABLE.
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  // EV_DISABLE should disable an enabled event.
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_DISABLE, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_ENABLE | EV_ONESHOT, 0, 0, 0);
  ASSERT_NE(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));

  auto lambda = [&]() {
    ASSERT_EQ(static_cast<int>(sizeof(a)), write(test_pipe[1], &a, sizeof(a)));
  };
  std::thread thread(lambda);

  // Triggering an one-shot event should disable it.
  ASSERT_EQ(1, kevent(kq_fd, nullptr, 0, &event, 1, nullptr));
  EXPECT_EQ(test_pipe[0], static_cast<int>(event.ident));
  EXPECT_EQ(EVFILT_READ, event.filter);
  EXPECT_EQ(static_cast<int>(sizeof(a)), event.data);

  // EV_DISABLE should fail on a triggered one-shot event.
  EV_SET(&event, test_pipe[0], EVFILT_READ, EV_DISABLE, 0, 0, 0);
  ASSERT_EQ(-1, kevent(kq_fd, &event, 1, nullptr, 0, nullptr));
  EXPECT_EQ(ENOENT, errno);

  thread.join();
  close(test_pipe[0]);
  close(test_pipe[1]);
}

}  // namespace net
}  // namespace dist_clang

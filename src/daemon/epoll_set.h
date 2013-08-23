#pragma once

#include <set>
#include <vector>

class Socket;

class EpollSet {
  public:
    struct Event {
      int fd;
      bool close_after_use;
    };

    EpollSet();
    ~EpollSet();

    bool HandleSocket(int fd);
    bool RearmSocket(int fd);
    int Wait(std::vector<Event>& events);

  private:
    bool MakeNonBlocking(int fd);
    bool IsListening(int fd);

    enum { BAD_SOCKET = -1 };

    int epoll_fd_;
    std::set<int> fds_;
};

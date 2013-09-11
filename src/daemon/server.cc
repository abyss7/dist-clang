#include "daemon/server.h"

#include "base/c_utils.h"
#include "daemon/epoll_set.h"
#include "proto/protobuf_utils.h"
#include "proto/remote.pb.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <memory>

using std::string;
using namespace google::protobuf::io;

Server::Server(size_t concurrency, size_t pool_size)
  : thread_pool_(pool_size, concurrency), network_threads_(concurrency),
    is_running_(false) {}

bool Server::Listen(const string& path, string* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    GetLastError(error);
    return false;
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    GetLastError(error);
    return false;
  }

  if (listen(fd, network_threads_.size()) == -1) {
    GetLastError(error);
    return false;
  }

  if (!epoll_set_.HandleSocket(fd)) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Server::Listen(const string& host, short int port, string* error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    GetLastError(error);
    return false;
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    GetLastError(error);
    return false;
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    GetLastError(error);
    return false;
  }

  if (listen(fd, network_threads_.size()) == -1) {
    GetLastError(error);
    return false;
  }

  if (!epoll_set_.HandleSocket(fd)) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Server::Run() {
  if (is_running_)
    // FIXME: consider, what should we return in this situation.
    return false;

  thread_pool_.Run();

  auto lambda = [this](std::thread& thread) {
    std::thread tmp(&Server::DoWork, this);
    thread.swap(tmp);
  };
  std::for_each(network_threads_.begin(), network_threads_.end(), lambda);

  is_running_ = true;
  return true;
}

void Server::DoWork() {
  while (true) {
    std::vector<EpollSet::Event> events;

    auto number_of_events = epoll_set_.Wait(events);
    if (number_of_events == -1)
      return;

    for (int i = 0; i < number_of_events; ++i)
      thread_pool_.Push(std::bind(&Server::HandleMessage, this,
                                  events[i].fd, events[i].close_after_use));
  }
}

void Server::HandleMessage(int fd, bool close_after_use) {
  std::unique_ptr<ZeroCopyInputStream> file_stream;
  std::unique_ptr<CodedInputStream> coded_stream;
  file_stream.reset(new FileInputStream(fd));
  coded_stream.reset(new CodedInputStream(file_stream.get()));

  dist_clang::Execute message;
  if ((!message.ParsePartialFromCodedStream(coded_stream.get()) ||
      !message.IsInitialized()) && !close_after_use)
    std::cout << "Failed to read message" << std::endl;
  else
    PrintMessage(message);

  // TODO: make local clang execution or delegate message to a remote daemon.

  if (!close_after_use)
    epoll_set_.RearmSocket(fd);
  else
    close(fd);
}

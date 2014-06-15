#pragma once

#include <base/aliases.h>
#include <net/connection_forward.h>

#include <netinet/in.h>

struct sockaddr;

namespace dist_clang {
namespace net {

class EndPoint : public std::enable_shared_from_this<EndPoint> {
 public:
  static EndPointPtr TcpHost(const std::string& host, ui16 port);
  static EndPointPtr UnixSocket(const std::string& path);

  operator const sockaddr*() const {
    return reinterpret_cast<const sockaddr*>(&address_);
  }

  socklen_t size() const { return size_; }

  int domain() const { return address_.ss_family; }

 private:
  sockaddr_storage address_;
  socklen_t size_;
};

}  // namespace net
}  // namespace dist_clang

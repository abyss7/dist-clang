#pragma once

#include "net/connection_forward.h"

#include <netinet/in.h>

struct sockaddr;

namespace dist_clang {
namespace net {

class EndPoint: public std::enable_shared_from_this<EndPoint> {
  public:
    static EndPointPtr TcpHost(const std::string& host, unsigned short port);
    static EndPointPtr UnixSocket(const std::string& path);

    operator const ::sockaddr* () const {
      return &address_;
    }

    size_t size() const {
      return sizeof(address_);
    }

    int domain() const {
      return address_.sa_family;
    }

  private:
    ::sockaddr address_;
};

}  // namespace net
}  // namespace dist_clang

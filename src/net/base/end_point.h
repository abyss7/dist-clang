#pragma once

#include "net/connection_forward.h"

#include <netinet/in.h>

struct sockaddr;

namespace dist_clang {
namespace net {

class EndPoint: public std::enable_shared_from_this<EndPoint> {
  public:
    static EndPointPtr TcpHost(const std::string& host, unsigned short port);

    operator const ::sockaddr* () const {
      return reinterpret_cast<const ::sockaddr*>(&in_address_);
    }

    size_t size() const {
      return sizeof(in_address_);
    }

  private:
    ::sockaddr_in in_address_;
};

}  // namespace net
}  // namespace dist_clang

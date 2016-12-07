#pragma once

#include <base/aliases.h>
#include <net/connection_forward.h>

#include <netinet/in.h>

struct sockaddr;

namespace dist_clang {
namespace net {

class EndPoint : public std::enable_shared_from_this<EndPoint> {
 public:
  virtual ~EndPoint() {}
  static EndPointPtr TcpHost(const String& host, ui16 port, bool ipv6);
  static EndPointPtr LocalHost(const String& host, ui16 port, bool ipv6);
  static EndPointPtr UnixSocket(const String& path);

  operator const sockaddr*() const {
    return reinterpret_cast<const sockaddr*>(&address_);
  }

  socklen_t size() const { return size_; }

  int domain() const { return address_.ss_family; }
  int type() const { return SOCK_STREAM; }
  int protocol() const { return protocol_; }

  virtual String Print() const;

 private:
  sockaddr_storage address_;
  socklen_t size_ = 0;
  int protocol_ = 0;
};

}  // namespace net
}  // namespace dist_clang

#include <net/end_point.h>

#include <base/assert.h>

#include <netdb.h>
#include <sys/un.h>

namespace dist_clang {
namespace net {

// static
EndPointPtr EndPoint::TcpHost(const String& host, ui16 port, bool ipv6) {
  struct addrinfo hints, *result;
  hints.ai_addr = nullptr;
  hints.ai_addrlen = 0;
  hints.ai_canonname = nullptr;
  if (ipv6) {
    hints.ai_family = AF_INET6;
  } else {
    hints.ai_family = AF_UNSPEC;
  }
  hints.ai_flags = 0;
  hints.ai_next = nullptr;
  hints.ai_protocol = 0;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == -1) {
    return EndPointPtr();
  }

  CHECK(result->ai_socktype == SOCK_STREAM);

  EndPointPtr end_point(new EndPoint);
  DCHECK(sizeof(end_point->address_) >= result->ai_addrlen);
  memcpy(&end_point->address_, result->ai_addr, result->ai_addrlen);
  end_point->size_ = result->ai_addrlen;
  end_point->protocol_ = result->ai_protocol;
  freeaddrinfo(result);

  if (ipv6) {
    auto* address = reinterpret_cast<sockaddr_in6*>(&end_point->address_);
    CHECK(address->sin6_family == AF_INET6);
    address->sin6_port = htons(port);
  } else {
    auto* address = reinterpret_cast<sockaddr_in*>(&end_point->address_);
    address->sin_port = htons(port);
  }

  return end_point;
}

// static
EndPointPtr EndPoint::UnixSocket(const String& path) {
  EndPointPtr end_point(new EndPoint);
  sockaddr_un* address = reinterpret_cast<sockaddr_un*>(&end_point->address_);
  address->sun_family = AF_UNIX;
  strncpy(address->sun_path, path.c_str(), sizeof(address->sun_path) - 1);
  end_point->size_ = sizeof(*address);
  return end_point;
}

}  // namespace net
}  // namespace dist_clang

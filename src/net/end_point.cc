#include <net/end_point.h>

#include <base/assert.h>
#include <base/logging.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>

#include <base/using_log.h>

namespace dist_clang {
namespace net {

namespace {

struct addrinfo* GetPeerAddress(const char* host, ui16 port, bool ipv6,
                                bool bind) {
  struct addrinfo hints, * result = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = ipv6 ? AF_INET6 : AF_INET;
  hints.ai_flags = bind ? AI_PASSIVE : 0;
  hints.ai_socktype = SOCK_STREAM;

  auto port_str = std::to_string(port);
  auto error = getaddrinfo(host, port_str.c_str(), &hints, &result);

  if (!error) {
    return result;
  }

  if (bind) {
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = ipv6 ? AF_INET6 : AF_INET;
    hints.ai_flags = bind ? AI_PASSIVE : 0;
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(nullptr, port_str.c_str(), &hints, &result);

    if (!error) {
      return result;
    }
  }

  LOG(ERROR) << "Failed to resolve \"" << host << ":" << port
             << "\": " << gai_strerror(error);

  return nullptr;
}

}  // namespace

// static
EndPointPtr EndPoint::TcpHost(const String& host, ui16 port, bool ipv6) {
  auto result = GetPeerAddress(host.c_str(), port, ipv6, false);
  if (!result) {
    return EndPointPtr();
  }

  CHECK(result->ai_socktype == SOCK_STREAM);
  CHECK(sizeof(EndPoint::address_) >= result->ai_addrlen);
  if (ipv6) {
    CHECK(result->ai_family == AF_INET6);
  }

  EndPointPtr end_point(new EndPoint);
  memcpy(&end_point->address_, result->ai_addr, result->ai_addrlen);
  end_point->size_ = result->ai_addrlen;
  end_point->protocol_ = result->ai_protocol;
  freeaddrinfo(result);

  return end_point;
}

// static
EndPointPtr EndPoint::LocalHost(const String& host, ui16 port, bool ipv6) {
  auto result = GetPeerAddress(host.c_str(), port, ipv6, true);
  if (!result) {
    return EndPointPtr();
  }

  CHECK(result->ai_socktype == SOCK_STREAM);
  CHECK(sizeof(EndPoint::address_) >= result->ai_addrlen);
  if (ipv6) {
    CHECK(result->ai_family == AF_INET6);
  }

  EndPointPtr end_point(new EndPoint);
  memcpy(&end_point->address_, result->ai_addr, result->ai_addrlen);
  end_point->size_ = result->ai_addrlen;
  end_point->protocol_ = result->ai_protocol;
  freeaddrinfo(result);

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

String EndPoint::Print() const {
  char buf[INET6_ADDRSTRLEN];
  auto* addr = reinterpret_cast<const struct sockaddr_in6*>(&address_);
  if (inet_ntop(domain(), &addr->sin6_addr, buf, INET6_ADDRSTRLEN)) {
    return buf;
  }
  return String();
}

}  // namespace net
}  // namespace dist_clang

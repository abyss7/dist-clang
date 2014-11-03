#include <net/end_point.h>

#include <netdb.h>
#include <sys/un.h>

namespace dist_clang {
namespace net {

// static
EndPointPtr EndPoint::TcpHost(const String& host, ui16 port) {
  hostent* host_entry;
  in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    return EndPointPtr();
  }

  address_list = reinterpret_cast<in_addr**>(host_entry->h_addr_list);

  EndPointPtr end_point(new EndPoint);
  sockaddr_in* address = reinterpret_cast<sockaddr_in*>(&end_point->address_);
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = address_list[0]->s_addr;
  address->sin_port = htons(port);
  end_point->size_ = sizeof(*address);
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

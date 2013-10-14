#include "net/base/end_point.h"

#include <string>

#include <netdb.h>

namespace dist_clang {
namespace net {

// static
EndPointPtr EndPoint::TcpHost(const std::string& host, unsigned short port) {
  ::hostent* host_entry;
  ::in_addr** address_list;

  if ((host_entry = ::gethostbyname(host.c_str())) == NULL) {
    return EndPointPtr();
  }

  address_list =
      reinterpret_cast<::in_addr**>(host_entry->h_addr_list);

  EndPointPtr end_point(new EndPoint);
  end_point->in_address_.sin_family = AF_INET;
  end_point->in_address_.sin_addr.s_addr = address_list[0]->s_addr;
  end_point->in_address_.sin_port = htons(port);
  return end_point;
}

}  // namespace net
}  // namespace dist_clang

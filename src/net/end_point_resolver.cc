#include <net/end_point_resolver.h>

#include <base/types.h>
#include <net/end_point.h>

namespace dist_clang {
namespace net {

EndPointResolver::Optional EndPointResolver::Resolve(const String& host,
                                                     ui16 port, bool ipv6) {
  Promise promise((EndPointPtr()));
  promise.SetValue(
      [host, port, ipv6] { return EndPoint::TcpHost(host, port, ipv6); });

  return promise.GetFuture();
}

}  // namespace net
}  // namespace dist_clang

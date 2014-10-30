#include <net/end_point_resolver.h>

#include <base/aliases.h>

namespace dist_clang {
namespace net {

EndPointResolver::Optional EndPointResolver::Resolve(const String& host,
                                                     ui16 port) {
  Promise promise((EndPointPtr()));
  promise.SetValue([host, port] { return EndPoint::TcpHost(host, port); });

  return promise.GetFuture();
}

}  // namespace net
}  // namespace dist_clang

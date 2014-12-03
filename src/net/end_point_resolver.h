#pragma once

#include <base/future.h>
#include <base/testable.h>
#include <net/connection_forward.h>

namespace dist_clang {
namespace net {

class EndPointResolver : public base::Testable<EndPointResolver> {
 public:
  using Promise = base::Promise<EndPointPtr>;
  using Optional = Promise::Optional;

  virtual ~EndPointResolver() {}

  // Resolve only remote tcp hosts, since there is no need for anything else
  // right now.
  virtual Optional Resolve(const String& host, ui16 port, bool ipv6);
};

}  // namespace net
}  // namespace dist_clang

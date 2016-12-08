#pragma once

#include <net/end_point.h>
#include <net/end_point_resolver.h>
#include <net/test_end_point.h>

namespace dist_clang {
namespace net {

class TestEndPointResolver : public EndPointResolver {
 public:
  class Factory : public EndPointResolver::Factory {
   public:
    UniquePtr<EndPointResolver> Create() override {
      return UniquePtr<EndPointResolver>(new TestEndPointResolver);
    }
  };

  Optional Resolve(const String& host, ui16 port, bool) override {
    return Promise(EndPointPtr(new TestEndPoint(host, port))).GetFuture();
  }
};

}  // namespace net
}  // namespace dist_clang

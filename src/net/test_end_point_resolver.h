#pragma once

#include <net/end_point_resolver.h>

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

  Optional Resolve(const String&, ui16, bool) override {
    return Promise((EndPointPtr())).GetFuture();
  }
};

}  // namespace net
}  // namespace dist_clang

#pragma once

#include <net/end_point_resolver.h>

namespace dist_clang {
namespace net {

class TestEndPointResolver : public EndPointResolver {
 public:
  class Factory : public EndPointResolver::Factory {
   public:
    virtual UniquePtr<EndPointResolver> Create() override {
      return UniquePtr<EndPointResolver>(new TestEndPointResolver);
    }
  };

  virtual Optional Resolve(const String&, ui16) override {
    return Promise((EndPointPtr())).GetFuture();
  }
};

}  // namespace net
}  // namespace dist_clang

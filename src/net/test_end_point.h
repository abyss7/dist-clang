#pragma once

#include <net/end_point.h>

namespace dist_clang {
namespace net {

class TestEndPoint : public EndPoint {
 public:
  TestEndPoint(const String& host) : host_(host) {}
  ~TestEndPoint() override {}
  String Print() const override {
    return host_;
  }
 private:
  String host_;
};

}  // namespace net
}  // namespace dist_clang

#pragma once

#include <net/end_point.h>

namespace dist_clang {
namespace net {

class TestEndPoint : public EndPoint {
 public:
  TestEndPoint(const String& host, ui16 port = 0u) : host_(host), port_(port) {}
  String Print() const override {
    return host_ + ":" + std::to_string(port_);
  }
 private:
  const String host_;
  const ui16 port_;
};

}  // namespace net
}  // namespace dist_clang

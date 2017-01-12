#pragma once

#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class Collector : public BaseDaemon {
 public:
  explicit Collector(const Configuration& conf);

  bool Initialize() override;

 private:
  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;
};

}  // namespace daemon
}  // namespace dist_clang

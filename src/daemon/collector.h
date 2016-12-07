#pragma once

#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class Collector : public BaseDaemon {
 public:
  explicit Collector(const proto::Configuration& configuration);

  bool Initialize() override;

 private:
  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  SharedPtr<const proto::Configuration> conf_;
};

}  // namespace daemon
}  // namespace dist_clang

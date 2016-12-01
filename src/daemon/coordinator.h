#pragma once

#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class Coordinator : public BaseDaemon {
 public:
  explicit Coordinator(const proto::Configuration& configuration);

  bool Initialize() override;
  inline bool UpdateConfiguration(
      const proto::Configuration& configuration) override {
    return true;
  }

 private:
  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  const proto::Host local_;
  Vector<proto::Host> remotes_;
};

}  // namespace daemon
}  // namespace dist_clang

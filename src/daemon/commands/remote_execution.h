#pragma once

#include "daemon/command.h"
#include "daemon/file_cache.h"       // for |FileCache::Entry|
#include "net/connection_forward.h"
#include "proto/remote.pb.h"         // for |message_|

namespace dist_clang {
namespace daemon {

class Daemon;

namespace command {

class RemoteExecution: public Command {
  public:
    static CommandPtr Create(
        net::ConnectionPtr connection,
        const proto::RemoteExecute& message,
        Daemon& daemon);

    virtual void Run() override;

  private:
    RemoteExecution(
        net::ConnectionPtr connection,
        const proto::RemoteExecute& message,
        Daemon& daemon);

    bool SearchCache(
        FileCache::Entry* entry);

    net::ConnectionPtr connection_;
    proto::RemoteExecute message_;
    Daemon& daemon_;
};

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

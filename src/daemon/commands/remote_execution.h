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
    using ScopedMessage = ::std::unique_ptr<proto::RemoteExecute>;

    static CommandPtr Create(
        net::ConnectionPtr connection,
        ScopedMessage message,
        Daemon& daemon);

    virtual void Run() override;

  private:
    RemoteExecution(
        net::ConnectionPtr connection,
        ScopedMessage message,
        Daemon& daemon);

    bool SearchCache(
        FileCache::Entry* entry);

    net::ConnectionPtr connection_;
    ScopedMessage message_;
    Daemon& daemon_;
};

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

#pragma once

#include "daemon/command.h"
#include "daemon/file_cache.h"       // for |FileCache::Entry|
#include "net/connection_forward.h"
#include "proto/remote.pb.h"         // for |message_|

#include <string>                    // for |pp_source_|

namespace dist_clang {
namespace daemon {

class Daemon;

namespace command {

class LocalExecution: public Command {
  public:
    using ScopedMessage = ::std::unique_ptr<proto::LocalExecute>;

    static CommandPtr Create(
        net::ConnectionPtr connection,
        ScopedMessage message,
        Daemon& daemon);

    virtual void Run() override;

  private:
    LocalExecution(
        net::ConnectionPtr connection,
        ScopedMessage message,
        Daemon& daemon);

    void DoneRemoteConnection(
        net::ConnectionPtr connection,
        const std::string& error);
    bool DoRemoteCompilation(
        net::ConnectionPtr connection,
        const proto::Status& status);
    bool DoneRemoteCompilation(
        net::ConnectionPtr connection,
        ::std::unique_ptr<proto::Universal> message,
        const proto::Status& status);

    void DoLocalCompilation();
    void DeferLocalCompilation();
    bool SearchCache(
        FileCache::Entry* entry);
    void UpdateCache(
        const proto::Status& status);
    inline std::shared_ptr<LocalExecution> shared_from_this();

    net::ConnectionPtr connection_;
    ScopedMessage message_;
    Daemon& daemon_;

    std::string pp_source_;
};

std::shared_ptr<LocalExecution> LocalExecution::shared_from_this() {
  return std::static_pointer_cast<LocalExecution>(Command::shared_from_this());
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

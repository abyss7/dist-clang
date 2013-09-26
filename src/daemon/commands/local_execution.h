#pragma once

#include "base/attributes.h"
#include "daemon/command.h"
#include "daemon/file_cache.h"       // for |FileCache::Entry|
#include "net/connection_forward.h"
#include "proto/remote.pb.h"         // for |message_|

#include <string>                    // for |pp_source_|

namespace dist_clang {
namespace daemon {

class Balancer;

namespace command {

class LocalExecution: public Command {
  public:
    static CommandPtr Create(
        net::ConnectionPtr connection,
        const proto::LocalExecute& message,
        Balancer* balancer,
        FileCache* cache);
    virtual void Run() override;

  private:
    LocalExecution(
        net::ConnectionPtr connection,
        const proto::LocalExecute& message,
        Balancer* balancer,
        FileCache* cache);

    bool DoRemoteCompilation(
        net::ConnectionPtr connection,
        const proto::Error& error);
    bool DoneRemoteCompilation(
        net::ConnectionPtr connection,
        const proto::Universal& message,
        const proto::Error& error);

    void DoLocalCompilation();
    bool SearchCache(
        FileCache::Entry* entry);
    void UpdateCache(
        const proto::Error& error);

    net::ConnectionPtr connection_;
    proto::LocalExecute message_;
    Balancer* balancer_ WEAK_PTR;
    FileCache* cache_ WEAK_PTR;

    std::string pp_source_;
};

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

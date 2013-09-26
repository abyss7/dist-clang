#pragma once

#include "base/attributes.h"
#include "daemon/command.h"
#include "daemon/file_cache.h"
#include "net/connection_forward.h"
#include "proto/remote.pb.h"

#include <string>

namespace dist_clang {
namespace daemon {

class Balancer;

namespace command {

class LocalExecution: public Command {
    using Error = proto::Error;
    using Local = proto::LocalExecute;
    using Message = proto::Universal;
    using Remote = proto::RemoteExecute;
    using Result = proto::RemoteResult;
    using This = LocalExecution;
  public:
    static CommandPtr Create(net::ConnectionPtr connection,
                             const Local& message,
                             Balancer* balancer,
                             FileCache* cache);
    virtual void Run() override;

  private:
    LocalExecution(net::ConnectionPtr connection,
                   const Local& message,
                   Balancer* balancer,
                   FileCache* cache);

    void DoLocalCompilation();
    bool DoRemoteCompilation(
        net::ConnectionPtr connection,
        const Error& error);
    bool DoneRemoteCompilation(
        net::ConnectionPtr connection,
        const Message& message,
        const Error& error);
    bool SearchCache(
        FileCache::Entry* entry);
    void UpdateCache(
        const Error& error);

    net::ConnectionPtr connection_;
    proto::LocalExecute message_;
    Balancer* balancer_ WEAK_PTR;
    FileCache* cache_ WEAK_PTR;

    std::string pp_source_;
};

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

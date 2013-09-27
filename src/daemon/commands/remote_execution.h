#pragma once

#include "base/attributes.h"
#include "daemon/command.h"
#include "daemon/file_cache.h"
#include "net/connection_forward.h"
#include "proto/remote.pb.h"

#include <unordered_map>

namespace dist_clang {

namespace proto {
class RemoteExecute;
}

namespace daemon {
namespace command {

class RemoteExecution: public Command {
  public:
    // The version is a key, and the compiler's path is a value.
    using CompilerMap = std::unordered_map<std::string, std::string>;

    static CommandPtr Create(net::ConnectionPtr connection,
                             const proto::RemoteExecute& remote,
                             FileCache* cache,
                             const CompilerMap* compilers);

    virtual void Run() override;

  private:
    RemoteExecution(net::ConnectionPtr connection,
                    const proto::RemoteExecute& remote,
                    FileCache* cache,
                    const CompilerMap* compilers);

    bool SearchCache(FileCache::Entry* entry);

    net::ConnectionPtr connection_;
    proto::RemoteExecute message_;
    FileCache* cache_ WEAK_PTR;
    const CompilerMap* compilers_ WEAK_PTR;
};

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

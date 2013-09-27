#pragma once

#include <memory>

namespace dist_clang {
namespace daemon {
namespace command {

class Command: public std::enable_shared_from_this<Command> {
  public:
    virtual void Run() = 0;
};

using CommandPtr = std::shared_ptr<Command>;

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

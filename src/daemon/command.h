#pragma once

#include <memory>
#if defined(PROFILER)
#  include <gperftools/profiler.h>
#endif  // PROFILER

namespace dist_clang {
namespace daemon {
namespace command {

class Command: public std::enable_shared_from_this<Command> {
  public:
    virtual void Run() = 0;
    virtual inline ~Command();
};

using CommandPtr = std::shared_ptr<Command>;

Command::~Command() {
#if defined(PROFILER)
  ProfilerFlush();
#endif  // PROFILER
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang

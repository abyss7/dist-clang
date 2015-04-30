#include <daemon/collector.h>

namespace dist_clang {
namespace daemon {

Collector::Collector(const proto::Configuration& configuration)
    : BaseDaemon(configuration) {
  // TODO: implement this.
}

bool Collector::Initialize() {
  // TODO: implement this.

  return BaseDaemon::Initialize();
}

}  // namespace daemon
}  // namespace dist_clang

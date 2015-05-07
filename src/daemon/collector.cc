#include <daemon/collector.h>

#include <base/logging.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

Collector::Collector(const proto::Configuration& configuration)
    : BaseDaemon(configuration), local_(configuration.collector().local()) {
  CHECK(configuration.has_collector());
}

bool Collector::Initialize() {
  String error;
  if (!Listen(local_.host(), local_.port(), local_.ipv6(), &error)) {
    LOG(ERROR) << "Failed to listen on " << local_.host() << ":"
               << local_.port() << " : " << error;
    return false;
  }

  return BaseDaemon::Initialize();
}

}  // namespace daemon
}  // namespace dist_clang

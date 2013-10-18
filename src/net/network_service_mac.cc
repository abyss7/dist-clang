#include "net/network_service.h"

#include "net/base/utils.h"
#include "net/kqueue_event_loop.h"

#include <sys/event.h>

using namespace ::std::placeholders;

namespace dist_clang {
namespace net {

bool NetworkService::Run() {
  auto old_signals = BlockSignals();

  // TODO: implement this.
  poll_fd_ = kqueue();
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
  pool_.reset(new WorkerPool);
  auto work = std::bind(&NetworkService::DoConnectWork, this, _1);
  pool_->AddWorker(work, concurrency_);

  UnblockSignals(old_signals);

  return event_loop_->Run();
}

}  // namespace net
}  // namespace dist_clang

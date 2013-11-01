#pragma once

namespace dist_clang {
namespace base {

class LockedQueueObserver {
  public:
    virtual ~LockedQueueObserver() {}

    // Do not remove observer inside |Observe()| - it will cause a deadlock.
    virtual void Observe(bool close) = 0;
};

}  // namespace base
}  // namespace dist_clang

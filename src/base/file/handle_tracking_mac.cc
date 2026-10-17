#include <base/file/handle_tracking.h>

namespace dist_clang {
namespace base {

// Unsupported: the lock of the tracking may deadlock on Mac with |fork()|.
void TrackHandle(int) {}
void UntrackHandle(int) {}

}  // namespace base
}  // namespace dist_clang

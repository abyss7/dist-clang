#pragma once

namespace dist_clang {
namespace base {

// Checks in the debug builds, that only one handle owns every file descriptor
// - the platforms, that can't, do nothing.
void TrackHandle(int fd);
void UntrackHandle(int fd);

}  // namespace base
}  // namespace dist_clang

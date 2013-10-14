#pragma once

struct timespec;

namespace dist_clang {
namespace base {

bool GetAbsoluteTime(::timespec& time);

}  // namespace base
}  // namespace dist_clang

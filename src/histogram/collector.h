#pragma once

#include "base/types.h"

namespace dist_clang {
namespace histogram {

class Collector {
 public:
  ui64 Start(const char* label);
  void Report(ui64 value);
  void Stop(ui64 id);
};

}  // namespace histogram
}  // namespace dist_clang

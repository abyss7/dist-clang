#include <histogram/collector.h>

#include <base/assert.h>

namespace dist_clang {
namespace histogram {

ui64 Collector::Start(const char* label) {
  auto new_id = next_id_.fetch_add(1);
  while (!labels_.emplace(new_id, label).second) {
    new_id = next_id_.fetch_add(1);
  }
  CHECK(intervals_.emplace(new_id, Interval{Clock::now(), .value = 0}).second);

  return new_id;
}

void Collector::Report(ui64 id, ui64 value) { intervals_[id].value += value; }

void Collector::Stop(ui64 id) { intervals_[id].stop = Clock::now(); }

}  // namespace histogram
}  // namespace dist_clang

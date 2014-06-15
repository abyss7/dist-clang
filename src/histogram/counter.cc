#include <histogram/counter.h>

#include <base/assert.h>
#include <base/singleton.h>
#include <histogram/collector.h>

namespace dist_clang {
namespace histogram {

Counter::Counter(const char* label)
    : id_(base::Singleton<Collector>::Get().Start(label)) {}

Counter::Counter(const char* label, ui64 value)
    : id_(base::Singleton<Collector>::Get().Start(label)), value_(value) {
  DCHECK(value);
}

Counter::~Counter() {
  if (value_) {
    base::Singleton<Collector>::Get().Report(value_);
  }
  base::Singleton<Collector>::Get().Stop(id_);
}

}  // namespace base
}  // namespace dist_clang

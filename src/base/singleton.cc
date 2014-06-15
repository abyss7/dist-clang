#include <base/singleton.h>

#include <histogram/collector.h>

#define DEFINE_SINGLETON(Class)                            \
  template <>                                              \
  UniquePtr<Class> Singleton<Class>::instance_ = {}; \
  template <>                                              \
  std::once_flag Singleton<Class>::once_flag_ = {};

namespace dist_clang {
namespace base {

DEFINE_SINGLETON(histogram::Collector);

}  // namespace base
}  // namespace dist_clang

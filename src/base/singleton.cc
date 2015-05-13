#include <base/singleton.h>

#include <perf/stat_service.h>

#define DEFINE_SINGLETON(Class)                            \
  template <>                                              \
  UniquePtr<Class> base::Singleton<Class>::instance_ = {}; \
  template <>                                              \
  std::once_flag base::Singleton<Class>::once_flag_ = {};

namespace dist_clang {

DEFINE_SINGLETON(perf::StatService)

}  // namespace dist_clang

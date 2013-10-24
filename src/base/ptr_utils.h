#pragma once

#include <memory>

namespace dist_clang {
namespace base {

template <typename Dst, typename Src>
inline ::std::unique_ptr<Dst> unique_static_cast(::std::unique_ptr<Src>& ptr) {
  Src* p = ptr.release();
  ::std::unique_ptr<Dst> r(static_cast<Dst*>(p));
  if (!r) {
    ptr.reset(p);
  }
  return r;
}

}  // namespace base
}  // namespace dist_clang

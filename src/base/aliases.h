#pragma once

#include <base/stl_include.h>

#if !defined(QT_CREATOR)
#include STL(atomic)
#include STL(cstdint)
#include STL(functional)
#include STL(list)
#include STL(memory)
#include STL(mutex)
#include STL(string)
#include STL(tuple)
#include STL(unordered_map)
#include STL(unordered_set)
#include STL(vector)
#else
#include <third_party/libcxx/exported/include/atomic>
#include <third_party/libcxx/exported/include/cstdint>
#include <third_party/libcxx/exported/include/functional>
#include <third_party/libcxx/exported/include/list>
#include <third_party/libcxx/exported/include/memory>
#include <third_party/libcxx/exported/include/mutex>
#include <third_party/libcxx/exported/include/string>
#include <third_party/libcxx/exported/include/tuple>
#include <third_party/libcxx/exported/include/unordered_map>
#include <third_party/libcxx/exported/include/unordered_set>
#include <third_party/libcxx/exported/include/vector>
#endif  // !defined(QT_CREATOR)

#if defined(OS_WIN)
#include STL(thread)
#else
#include <base/thread.h>
#endif

namespace dist_clang {

namespace base {
class ConstString;
class Literal;
}  // namespace base

using i8 = int8_t;
using ui8 = uint8_t;
using i16 = int16_t;
using ui16 = uint16_t;
using i32 = int32_t;
using ui32 = uint32_t;
using i64 = int64_t;
using ui64 = uint64_t;

template <class T>
using Atomic = std::atomic<T>;

using Clock = std::chrono::steady_clock;

using Immutable = base::ConstString;

template <typename Signature>
using Fn = std::function<Signature>;

template <class U, class V>
using HashMap = std::unordered_map<U, V>;

template <class U>
using HashSet = std::unordered_set<U>;

template <class T>
using List = std::list<T>;

using Literal = base::Literal;

template <class U, class V = U>
using Pair = std::pair<U, V>;

template <class T>
using SharedPtr = std::shared_ptr<T>;

using String = std::string;

#if defined(OS_WIN)
using Thread = std::thread;
#else
using Thread = base::Thread;
#endif  // defined(OS_WIN)

using TimePoint = std::chrono::time_point<Clock>;

template <typename... Args>
using Tuple = std::tuple<Args...>;

using UniqueLock = std::unique_lock<std::mutex>;

template <class T>
using UniquePtr = std::unique_ptr<T>;

template <class T>
using Vector = std::vector<T>;

template <class T>
using WeakPtr = std::weak_ptr<T>;

}  // namespace dist_clang

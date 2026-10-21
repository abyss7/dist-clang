#pragma once

#include <base/types.h>

#include STL(tuple)

struct sqlite3;

namespace dist_clang {
namespace cache {

// Opens the database. If it's corrupted - e.g. by a crash of the system - the
// database is removed and created anew: it's only an index of the cache, and
// the missing entries are just cache misses.
// Sets |created|, if the database is new: it didn't exist, or was broken.
// The |full_check| reads the whole database - for the starts after a crash.
// Otherwise only the header is checked.
sqlite3* OpenDatabase(const String& path, bool* created = nullptr,
                      bool full_check = true);

// Keeps the transactions of the |databases| open in the scope, and ends them in
// the reverse order.
template <class... Databases>
class Transaction {
 public:
  explicit Transaction(Databases*... databases) : databases_(databases...) {
    (databases->BeginTransaction(), ...);
  }
  ~Transaction() { End<sizeof...(Databases)>(); }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

 private:
  template <size_t N>
  void End() {
    if constexpr (N > 0) {
      std::get<N - 1>(databases_)->EndTransaction();
      End<N - 1>();
    }
  }

  std::tuple<Databases*...> databases_;
};

}  // namespace cache
}  // namespace dist_clang

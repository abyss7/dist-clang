#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <cache/file_hash_cache.h>

namespace dist_clang {
namespace daemon {

// Identifies compilers by their binaries and by their default configuration.
class CompilerIdentity {
 public:
  // Returns the identity of the compiler that is run as |path|: the name of
  // the executable matters, since it may set the driver mode, e.g. "clang++".
  bool Get(const String& path, String* identity,
           String* error = nullptr) THREAD_SAFE;

  // Finds the executable |name| like a shell does, using the |env| of the
  // client. Returns an empty string if it's not found.
  static String Resolve(const String& name, const String& current_dir,
                        const List<String>& env);

 private:
  cache::FileHashCache binaries_;

  Mutex mutex_;
  HashMap<String /* path + binary hash */, String /* identity */> identities_;
};

}  // namespace daemon
}  // namespace dist_clang

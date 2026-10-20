#pragma once

#include <base/attributes.h>
#include <base/base.pb.h>
#include <base/types.h>
#include <cache/file_hash_cache.h>

#include STL(condition_variable)

namespace dist_clang {
namespace daemon {

// Identifies compilers by their binaries and by their default configuration.
class CompilerIdentity {
 public:
  // The identity is taken again after |ttl|.
  explicit CompilerIdentity(
      std::chrono::milliseconds ttl = std::chrono::minutes(1))
      : ttl_(ttl) {}

  // Returns the identity of the compiler that is run as |path|: the name of
  // the executable matters, since it may set the driver mode, e.g. "clang++".
  bool Get(const String& path, String* identity,
           String* error = nullptr) THREAD_SAFE;

  // Returns the include directories, that the compiler |path| searches by
  // itself for the compilation with |flags|: they depend on the language and
  // on some flags, e.g. "--sysroot" or "-stdlib".
  bool GetIncludeDirs(const String& path, const String& identity,
                      const base::proto::Flags& flags, List<String>* dirs,
                      String* error = nullptr) THREAD_SAFE;

  // The include directories from the output of "-v".
  static List<String> ParseIncludeDirs(const String& output);

  // Finds the executable |name| like a shell does, using the |env| of the
  // client. Returns an empty string if it's not found.
  static String Resolve(const String& name, const String& current_dir,
                        const List<String>& env);

 private:
  struct Identity {
    String identity;
    TimePoint time;
    bool updating = false;
    // Taken for the first time - the other compilations wait for it.
    bool taking = false;
  };

  // The identity by the dry runs of the compiler.
  bool Take(const String& path, const String& binary_hash, String* identity,
            String* error);

  const std::chrono::milliseconds ttl_;
  cache::FileHashCache binaries_;

  Mutex mutex_;
  std::condition_variable taken_;
  HashMap<String /* path + binary hash */, Identity> identities_;
  HashMap<String /* identity + language + flags */, List<String>>
      include_dirs_;
};

}  // namespace daemon
}  // namespace dist_clang

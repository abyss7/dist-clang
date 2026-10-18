#pragma once

#include <base/attributes.h>
#include <base/base.pb.h>
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
  cache::FileHashCache binaries_;

  Mutex mutex_;
  HashMap<String /* path + binary hash */, String /* identity */> identities_;
  HashMap<String /* identity + language + flags */, List<String>>
      include_dirs_;
};

}  // namespace daemon
}  // namespace dist_clang

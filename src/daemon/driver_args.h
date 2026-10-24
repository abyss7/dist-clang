#pragma once

#include <base/types.h>

namespace dist_clang {

namespace base {
namespace proto {
class Flags;
}  // namespace proto
}  // namespace base

namespace daemon {

// Classifies the arguments of a compiler driver (without the compiler itself)
// into |flags|. Works on the level of the public driver interface, so it
// doesn't depend on the compiler's version: unknown flags are considered to
// affect the result and get into the cache key as is.
//
// Returns |false| if the command can't be handled by the daemon - and should
// be run as is - with a human-readable |reason|.
bool ParseDriverArgs(const List<String>& args, const String& current_dir,
                     base::proto::Flags* flags, String* reason);

// Replaces the arguments like "@file" with the contents of the response file.
// Relative paths are resolved against |current_dir|.
bool ExpandResponseFiles(List<String>& args, const String& current_dir,
                         String* error);

// Adds to |flags| the cache key parts from the compiler's environment |env|.
// Returns |false| if the environment makes the command unsupported.
// If the flag changes only the format of the diagnostics - not the colors.
bool IsDiagnosticsFormatFlag(const String& arg);

bool ParseEnvironment(const List<String>& env, base::proto::Flags* flags,
                      String* reason);

// The current directory, that the compiler sees: from PWD of its environment
// in |flags|, if it's the same directory as |current_dir| - e.g. through a
// symlink. The compiler writes the paths with it.
String CompilerCurrentDir(const base::proto::Flags& flags,
                          const String& current_dir);

// Escapes |target| like the compiler does for "-MQ" flag.
String QuoteMakeTarget(const String& target);

}  // namespace daemon
}  // namespace dist_clang

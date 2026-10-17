#pragma once

#include <base/const_string.h>
#include <base/types.h>

struct stat;

namespace dist_clang {
namespace base {

// The functions, that differ between the platforms. Every platform has its own
// implementation - "platform_<os>.cc": the build takes the one of the target
// platform. The things, that a platform doesn't support yet, report it and do
// nothing.

enum class Platform { LINUX, MACOSX };
Platform CurrentPlatform();

// The time of the modification of the file contents, and of the file status
// (e.g. of the hard links), in nanoseconds since the epoch.
i64 ModificationTime(const struct stat& st);
i64 StatusChangeTime(const struct stat& st);

// The current time in nanoseconds since the epoch - from the clock, that the
// file systems take the times of the modifications from: the file, modified
// after this time, has a later time of the modification.
i64 FileSystemTime();

// Opens the unique file by the |path_template| with "XXXXXX" before the suffix
// of |suffix_size| characters, that isn't inherited by the child processes.
// Replaces the "XXXXXX". Returns the file descriptor, or -1.
int CreateUniqueFile(char* path_template, int suffix_size);

// The directory of the executable of the current process.
bool GetSelfPath(String& result, String* error = nullptr);

void SetCurrentThreadName(Literal name);
// The current thread works in background: e.g. it shouldn't slow down the
// compilations.
void LowerCurrentThreadPriority();

// Detaches the process from the terminal, and works further in background.
bool Daemonize(String* error = nullptr);

}  // namespace base
}  // namespace dist_clang

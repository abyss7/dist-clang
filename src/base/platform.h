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

// The current time in nanoseconds since the epoch - comparable with the times
// of the modifications of the files: the file, modified after the call, has a
// later time of the modification - also on the file systems, that take the
// times from the coarse clock. The files, modified shortly before the call,
// may have a later time too.
i64 FileSystemTime();

// The file |path| with the status |st| shares its storage with other files:
// it has other hard links, or its blocks are shared with copy-on-write clones.
// Its removal doesn't free the space then.
bool SharesStorage(const Path& path, const struct stat& st);

// Opens the unique file by the |path_template| with "XXXXXX" before the suffix
// of |suffix_size| characters, that isn't inherited by the child processes.
// Replaces the "XXXXXX". Returns the file descriptor, or -1.
int CreateUniqueFile(char* path_template, int suffix_size);

// The key of the |path| for the comparisons: the same for the spellings of the
// same file - e.g. in another case on the file systems, that ignore it, like
// the default one on Mac.
String PathKey(const String& path);

// The executable of the current process, and its directory.
bool GetSelfExecutable(String& result, String* error = nullptr);
bool GetSelfPath(String& result, String* error = nullptr);

void SetCurrentThreadName(Literal name);
// The current thread works in background: e.g. it shouldn't slow down the
// compilations.
void LowerCurrentThreadPriority();

// Detaches the process from the terminal, and works further in background.
bool Daemonize(String* error = nullptr);

}  // namespace base
}  // namespace dist_clang

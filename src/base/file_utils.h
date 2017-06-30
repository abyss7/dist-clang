#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

void WalkDirectory(
    const Path& path,
    Fn<void(const Path& file_path, ui64 mtime, ui64 size)> visitor,
    String* error = nullptr);

ui64 CalculateDirectorySize(const Path& path, String* error = nullptr);

bool RemoveEmptyDirectory(const Path& path);

bool ChangeOwner(const Path& path, ui32 uid, String* error = nullptr);

bool CreateDirectory(const Path& path, String* error = nullptr);

Path AppendExtension(Path path, const char* extension);

}  // namespace base
}  // namespace dist_clang

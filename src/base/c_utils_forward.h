#pragma once

#include <base/aliases.h>
#include <base/const_string.h>

namespace dist_clang {
namespace base {

bool ChangeCurrentDir(const Path& path, String* error = nullptr);
Path CreateTempFile(String* error = nullptr);
Path CreateTempFile(const char suffix[], String* error = nullptr);
Path GetCurrentDir(String* error = nullptr);
Literal GetEnv(Literal env_name, Literal default_env = Literal::empty);
Literal GetHomeDir(String* error = nullptr);
void GetLastError(String* error);
bool GetSelfPath(String& result, String* error = nullptr);
Literal SetEnv(Literal env_name, const String& value, String* error = nullptr);
bool SetPermissions(const Path& path, const Perms permissions,
                    String* error = nullptr);

}  // namespace base
}  // namespace dist_clang

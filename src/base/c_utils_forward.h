#pragma once

#include <base/aliases.h>
#include <base/const_string.h>

namespace dist_clang {
namespace base {

bool ChangeCurrentDir(Immutable path, String* error = nullptr);
String CreateTempFile(String* error = nullptr);
String CreateTempFile(const char suffix[], String* error = nullptr);
Immutable GetCurrentDir(String* error = nullptr);
Literal GetEnv(Literal env_name, Literal default_env = Literal::empty);
Literal GetHomeDir(String* error = nullptr);
void GetLastError(String* error);
String GetSelfPath(String* error = nullptr);
Literal SetEnv(Literal env_name, const String& value, String* error = nullptr);
bool SetPermissions(const String& path, int mask, String* error = nullptr);

}  // namespace base
}  // namespace dist_clang

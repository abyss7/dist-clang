#pragma once

#include <base/types.h>
#include <base/const_string.h>

namespace dist_clang {
namespace base {

String CreateTempFile(String* error = nullptr);
String CreateTempFile(const char suffix[], String* error = nullptr);
Literal GetEnv(Literal env_name, Literal default_env = Literal::empty);
Path GetHomeDir(String* error = nullptr);
void GetLastError(String* error);
bool GetSelfPath(String& result, String* error = nullptr);
Literal SetEnv(Literal env_name, const String& value, String* error = nullptr);
bool SetPermissions(const String& path, int mask, String* error = nullptr);

}  // namespace base
}  // namespace dist_clang

#include <base/constants.h>

namespace dist_clang {
namespace base {

Literal kDefaultSocketPath = "/tmp/clangd.socket"_l;
Literal kEnvClangPath = "DC_CLANG_PATH"_l;
Literal kEnvClangVersion = "DC_CLANG_VERSION"_l;
Literal kEnvDisabled = "DC_DISABLED"_l;
Literal kEnvLogLevels = "DC_LOG_LEVELS"_l;
Literal kEnvLogErrorMark = "DC_LOG_ERROR_MARK"_l;
Literal kEnvSocketPath = "DC_SOCKET_PATH"_l;

}  // namespace base
}  // namespace dist_clang

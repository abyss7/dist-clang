#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], Immutable socket_path,
            Immutable clang_path, Immutable version);

}  // namespace client
}  // namespace dist_clang

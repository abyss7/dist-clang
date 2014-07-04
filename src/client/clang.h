#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], const String& socket_path);

}  // namespace client
}  // namespace dist_clang

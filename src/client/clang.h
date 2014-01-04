#pragma once

#include <string>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], const std::string& socket_path,
            const std::string& clang_path);

}  // namespace client
}  // namespace dist_clang

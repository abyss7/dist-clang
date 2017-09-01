#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], Immutable socket_path,
            const Path& clang_path, Immutable version,
            ui32 connect_timeout_secs, ui32 read_timeout_secs,
            ui32 send_timeout_secs, ui32 read_min_bytes,
            const HashMap<String, String>& plugins, bool disabled,
            bool rewrite_includes);

}  // namespace client
}  // namespace dist_clang

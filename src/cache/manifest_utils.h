#pragma once

#include <base/aliases.h>

namespace dist_clang {

namespace proto {
class Manifest;
}

namespace cache {

bool LoadManifest(const String& path, proto::Manifest* manifest);
bool SaveManifest(const String& path, const proto::Manifest& manifest);

}  // namespace cache
}  // namespace dist_clang

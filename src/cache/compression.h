#pragma once

#include <base/const_string.h>
#include <cache/manifest.pb.h>

namespace dist_clang {
namespace cache {

// Compresses |data| for the cache - at the fast level, since it's on the way
// of the compilation.
bool Pack(proto::Compression compression, Immutable data, String* packed);
bool Unpack(proto::Compression compression, Immutable data, String* unpacked);

}  // namespace cache
}  // namespace dist_clang

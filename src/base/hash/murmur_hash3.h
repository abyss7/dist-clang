//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.
//-----------------------------------------------------------------------------

#pragma once

#include <stdint.h>

namespace dist_clang {
namespace base {
namespace hash_details {

void MurmurHash3_x86_32(const void* key, int len, uint32_t seed, void* out );
void MurmurHash3_x86_128(const void* key, int len, uint32_t seed, void* out );
void MurmurHash3_x64_128(const void* key, int len, uint32_t seed, void* out );

}  // namespace hash_details
}  // namespace base
}  // namespace dist_clang

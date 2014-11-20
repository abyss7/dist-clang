#include <base/hash.h>

#include <base/const_string.h>

namespace dist_clang {

namespace {

inline ui64 rotl64(ui64 x, i8 r) {
  return (x << r) | (x >> (64 - r));
}

inline ui64 fmix64(ui64 k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdLLU;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53LLU;
  k ^= k >> 33;

  return k;
}

}  // namespace

namespace base {

Immutable MakeHash(Immutable input, ui8 size) {
  char* buf = new char[16];
  const ui64 nblocks = input.size() / 16;
  ui64 h1 = 0, h2 = 0;
  const ui64 c1 = 0x87c37b91114253d5LLU, c2 = 0x4cf5ad432745937fLLU;

  // Body.

  for (ui64 i = 0; i < nblocks; ++i) {
    ui64 k1 = input.GetBlock64(i * 2 + 0), k2 = input.GetBlock64(i * 2 + 1);

    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;

    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;

    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;

    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  // Tail.

  const ui64 tail = nblocks * 16;

  ui64 k1 = 0, k2 = 0;

  switch (input.size() & 15) {
    case 15:
      k2 ^= ((ui64)input[tail + 14]) << 48;
    case 14:
      k2 ^= ((ui64)input[tail + 13]) << 40;
    case 13:
      k2 ^= ((ui64)input[tail + 12]) << 32;
    case 12:
      k2 ^= ((ui64)input[tail + 11]) << 24;
    case 11:
      k2 ^= ((ui64)input[tail + 10]) << 16;
    case 10:
      k2 ^= ((ui64)input[tail + 9]) << 8;
    case 9:
      k2 ^= ((ui64)input[tail + 8]) << 0;
      k2 *= c2;
      k2 = rotl64(k2, 33);
      k2 *= c1;
      h2 ^= k2;

    case 8:
      k1 ^= ((ui64)input[tail + 7]) << 56;
    case 7:
      k1 ^= ((ui64)input[tail + 6]) << 48;
    case 6:
      k1 ^= ((ui64)input[tail + 5]) << 40;
    case 5:
      k1 ^= ((ui64)input[tail + 4]) << 32;
    case 4:
      k1 ^= ((ui64)input[tail + 3]) << 24;
    case 3:
      k1 ^= ((ui64)input[tail + 2]) << 16;
    case 2:
      k1 ^= ((ui64)input[tail + 1]) << 8;
    case 1:
      k1 ^= ((ui64)input[tail + 0]) << 0;
      k1 *= c1;
      k1 = rotl64(k1, 31);
      k1 *= c2;
      h1 ^= k1;
  };

  // Finalization.

  h1 ^= input.size();
  h2 ^= input.size();

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  ((ui64*)buf)[0] = h1;
  ((ui64*)buf)[1] = h2;

  return Immutable(buf, std::min<ui8>(16u, size));
}

}  // namespace base
}  // namespace dist_clang

#include <cache/compression.h>

#include <snappy.h>
#include <zstd.h>

namespace dist_clang {
namespace cache {

bool Pack(proto::Compression compression, Immutable data, String* packed) {
  switch (compression) {
    case proto::NONE:
      packed->assign(data.data(), data.size());
      return true;
    case proto::SNAPPY:
      return snappy::Compress(data.data(), data.size(), packed);
    case proto::ZSTD: {
      packed->resize(ZSTD_compressBound(data.size()));
      const size_t size = ZSTD_compress(packed->data(), packed->size(),
                                        data.data(), data.size(), 1);
      if (ZSTD_isError(size)) {
        return false;
      }
      packed->resize(size);
      return true;
    }
  }
  return false;
}

bool Unpack(proto::Compression compression, Immutable data,
            String* unpacked) {
  switch (compression) {
    case proto::NONE:
      unpacked->assign(data.data(), data.size());
      return true;
    case proto::SNAPPY:
      return snappy::Uncompress(data.data(), data.size(), unpacked);
    case proto::ZSTD: {
      const auto size = ZSTD_getFrameContentSize(data.data(), data.size());
      if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) {
        return false;
      }
      unpacked->resize(size);
      const size_t actual_size = ZSTD_decompress(
          unpacked->data(), unpacked->size(), data.data(), data.size());
      return !ZSTD_isError(actual_size) && actual_size == size;
    }
  }
  return false;
}

}  // namespace cache
}  // namespace dist_clang

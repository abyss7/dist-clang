#include <cache/compression.h>

#include <zstd.h>

namespace dist_clang {
namespace cache {

bool Pack(proto::Compression compression, Immutable data, String* packed) {
  switch (compression) {
    case proto::NONE:
      packed->assign(data.data(), data.size());
      return true;
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
    case proto::ZSTD: {
      const auto size = ZSTD_getFrameContentSize(data.data(), data.size());
      if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) {
        return false;
      }
      // A broken header mustn't take all the memory: the contents grow only
      // with the decompressed data - and can't exceed the size in the header.
      std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> context(
          ZSTD_createDCtx(), ZSTD_freeDCtx);
      if (!context) {
        return false;
      }
      unpacked->clear();
      unpacked->reserve(std::min<ui64>(size, 256u << 20));
      ZSTD_inBuffer input = {data.data(), data.size(), 0};
      String chunk(ZSTD_DStreamOutSize(), '\0');
      size_t result = 1;
      while (result != 0) {
        ZSTD_outBuffer output = {chunk.data(), chunk.size(), 0};
        result = ZSTD_decompressStream(context.get(), &output, &input);
        if (ZSTD_isError(result) || unpacked->size() + output.pos > size) {
          return false;
        }
        unpacked->append(chunk.data(), output.pos);
        // The input has ended before the frame.
        if (result != 0 && input.pos == input.size && output.pos == 0) {
          return false;
        }
      }
      return unpacked->size() == size;
    }
  }
  return false;
}

}  // namespace cache
}  // namespace dist_clang

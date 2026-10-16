#include <cache/blob_store.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/compression.h>

#include <sys/stat.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

const char kPackedExtension[] = ".zst";

// Snappy is only for the old entries.
proto::Compression BlobCompression(proto::Compression compression) {
  return compression == proto::NONE ? proto::NONE : proto::ZSTD;
}

bool HasHash(Immutable content, const String& hash) {
  return base::Hexify(content.Hash()) == hash;
}

}  // namespace

BlobStore::BlobStore(const Path& root, proto::Compression compression,
                     bool link)
    : root_(root), compression_(BlobCompression(compression)), link_(link) {}

Path BlobStore::RawPath(const String& hash) const {
  return root_ / hash.substr(0, 2) / hash;
}

Path BlobStore::PackedPath(const String& hash) const {
  return root_ / hash.substr(0, 2) / (hash + kPackedExtension);
}

bool BlobStore::Put(Immutable content, const Path& path, String* hash,
                    bool* reused, String* error) const {
  *reused = false;

  auto Exists = [this](const String& hash) {
    return base::File::Exists(RawPath(hash)) ||
           base::File::Exists(PackedPath(hash));
  };

  // Link the produced file.
  if (link_ && content.empty() && !path.empty()) {
    Immutable file_hash;
    if (!base::File::Hash(path, &file_hash, {}, error)) {
      return false;
    }
    hash->assign(file_hash.string_copy());
    if (Exists(*hash)) {
      *reused = true;
      return true;
    }
    return base::CreateDirectory(RawPath(*hash).parent_path(), error) &&
           base::File::Clone(path, RawPath(*hash), true, nullptr, error);
  }

  Immutable data = content;
  if (data.empty() && !path.empty() &&
      !base::File::Read(path, &data, error)) {
    return false;
  }
  hash->assign(base::Hexify(data.Hash()));
  if (Exists(*hash)) {
    *reused = true;
    return true;
  }
  if (!base::CreateDirectory(RawPath(*hash).parent_path(), error)) {
    return false;
  }

  // The linked blobs aren't compressed, until they're unused.
  if (link_ || compression_ == proto::NONE) {
    return base::File::Write(RawPath(*hash), data, error);
  }
  String packed;
  if (!Pack(compression_, data, &packed)) {
    if (error) {
      error->assign("failed to pack contents");
    }
    return false;
  }
  return base::File::Write(PackedPath(*hash), Immutable(std::move(packed)),
                           error);
}

bool BlobStore::Get(const String& hash, Immutable* content, Path* path,
                    ui64* size) const {
  auto Broken = [](const Path& path, const char* reason) {
    LOG(CACHE_WARNING) << "Cache file is broken (" << reason << "): " << path;
    base::File::Delete(path);
    return false;
  };

  const Path raw_path = RawPath(hash);
  struct stat st;
  if (stat(raw_path.c_str(), &st) == 0) {
    // The file may be shared through a hard link - and modified in place by
    // someone else. Check the contents always: it's cheap compared to the
    // compilation.
    if (link_ && path) {
      Immutable actual_hash;
      if (!base::File::Hash(raw_path, &actual_hash) ||
          actual_hash.string_copy() != hash) {
        return Broken(raw_path, "wrong hash");
      }
      *path = raw_path;
      *size += st.st_size;
      return true;
    }

    Immutable data;
    if (!base::File::Read(raw_path, &data)) {
      return false;
    }
    if (!HasHash(data, hash)) {
      return Broken(raw_path, "wrong hash");
    }
    *content = data;
    *size += data.size();
    return true;
  }

  const Path packed_path = PackedPath(hash);
  Immutable packed;
  if (!base::File::Read(packed_path, &packed)) {
    return false;
  }
  String unpacked;
  if (!Unpack(proto::ZSTD, packed, &unpacked)) {
    return Broken(packed_path, "can't unpack");
  }
  Immutable data(std::move(unpacked));
  if (!HasHash(data, hash)) {
    return Broken(packed_path, "wrong hash");
  }
  *content = data;
  *size += packed.size();
  return true;
}

void BlobStore::AddRef(const String& hash, ui64 access_time) {
  auto& blob = blobs_[hash];
  ++blob.refs;
  blob.access_time = std::max(blob.access_time, access_time);
}

void BlobStore::Touch(const String& hash, ui64 access_time) {
  auto it = blobs_.find(hash);
  if (it != blobs_.end()) {
    it->second.access_time = std::max(it->second.access_time, access_time);
  }
}

void BlobStore::RemoveRef(const String& hash) {
  auto it = blobs_.find(hash);
  if (it == blobs_.end()) {
    return;
  }
  // A new entry may reuse the blob meanwhile - then it's just not found.
  if (--it->second.refs == 0) {
    blobs_.erase(it);
    Delete(hash);
  }
}

void BlobStore::Delete(const String& hash) {
  for (const auto& path : {RawPath(hash), PackedPath(hash)}) {
    if (base::File::Exists(path)) {
      base::File::Delete(path);
    }
  }
}

ui32 BlobStore::RemoveUnreferenced(ui64 time) {
  ui32 removed = 0;
  base::WalkDirectory(root_, [&](const Path& path, ui64 mtime, ui64) {
    String hash = path.filename().string();
    if (hash.ends_with(kPackedExtension)) {
      hash.resize(hash.size() - strlen(kPackedExtension));
    }
    if (hash.size() != 32 || blobs_.contains(hash)) {
      return;
    }
    // The blobs, that are just put, aren't counted yet.
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && ui64(st.st_ctime) < time &&
        mtime < time) {
      LOG(CACHE_INFO) << "Removing unreferenced blob " << path;
      base::File::Delete(path);
      ++removed;
    }
  });
  return removed;
}

ui32 BlobStore::CompressUnused(ui64 time, ui64 max_size,
                               Clock::time_point deadline) {
  if (compression_ == proto::NONE) {
    return 0;
  }

  ui32 compressed = 0;
  ui64 compressed_size = 0;
  for (const auto& [hash, blob] : blobs_) {
    if (compressed_size >= max_size || Clock::now() >= deadline) {
      break;
    }
    if (blob.access_time > time) {
      continue;
    }

    // The blob, that is linked with other places, takes no space.
    const Path raw_path = RawPath(hash);
    struct stat st;
    if (stat(raw_path.c_str(), &st) != 0 || st.st_nlink > 1) {
      continue;
    }

    Immutable data;
    String packed, error;
    if (!base::File::Read(raw_path, &data, &error) || !HasHash(data, hash) ||
        !Pack(compression_, data, &packed)) {
      continue;
    }
    // The raw blob is found first, until it's removed.
    if (!base::File::Write(PackedPath(hash), Immutable(std::move(packed)),
                           &error)) {
      LOG(CACHE_ERROR) << "Failed to compress " << raw_path << ": " << error;
      continue;
    }
    base::File::Delete(raw_path);
    compressed_size += data.size();
    ++compressed;
  }

  return compressed;
}

ui64 BlobStore::Size(const String& hash) const {
  struct stat st;
  if (stat(RawPath(hash).c_str(), &st) == 0 ||
      stat(PackedPath(hash).c_str(), &st) == 0) {
    return st.st_size;
  }
  return 0;
}

bool BlobStore::IsShared(const String& hash) const {
  struct stat st;
  return stat(RawPath(hash).c_str(), &st) == 0 && st.st_nlink > 1;
}

ui32 BlobStore::Refs(const String& hash) const {
  auto it = blobs_.find(hash);
  return it == blobs_.end() ? 0 : it->second.refs;
}

void BlobStore::GetTotalSize(ui64* own_size, ui64* shared_size) const {
  for (const auto& [hash, blob] : blobs_) {
    (IsShared(hash) ? *shared_size : *own_size) += Size(hash);
  }
}

}  // namespace cache
}  // namespace dist_clang

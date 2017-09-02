#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <cache/file_cache.h>
#include <cache/manifest.pb.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

bool Version_0_to_1(const Path& common_prefix, ui32 to_version,
                    proto::Manifest& manifest, bool& modified) {
  if (manifest.version() != 0 || to_version < 1) {
    return true;
  }

  DCHECK(!manifest.has_direct());
  DCHECK(!manifest.has_v1());

  if (manifest.headers_size()) {
    manifest.mutable_direct()->mutable_headers()->Swap(
        manifest.mutable_headers());
  } else {
    auto* v1 = manifest.mutable_v1();
    ui64 size = 0;

    v1->set_snappy(manifest.snappy());
    v1->set_err(manifest.stderr());
    v1->set_obj(manifest.object());
    v1->set_dep(manifest.deps());

    if (manifest.v1().err()) {
      const auto err_path = AppendExtension(common_prefix, base::kExtStderr);
      if (base::File::Exists(err_path)) {
        size += base::File::Size(err_path);
      } else {
        return false;
      }
    }
    if (manifest.v1().obj()) {
      const auto obj_path = AppendExtension(common_prefix, base::kExtObject);
      if (base::File::Exists(obj_path)) {
        size += base::File::Size(obj_path);
      } else {
        return false;
      }
    }
    if (manifest.v1().dep()) {
      const auto dep_path = AppendExtension(common_prefix, base::kExtDeps);
      if (base::File::Exists(dep_path)) {
        size += base::File::Size(dep_path);
      } else {
        return false;
      }
    }
    v1->set_size(size);
  }

  manifest.clear_headers();
  manifest.clear_snappy();
  manifest.clear_stderr();
  manifest.clear_object();
  manifest.clear_deps();

  manifest.set_version(1);

  modified = true;
  return true;
}

// Remove old direct cache entries since they contain absolute paths. And we
// can't distinguish which paths should shortened and which not.
bool Version_1_to_2(const Path& common_prefix, ui32 to_version,
                    proto::Manifest& manifest, bool& modified) {
  if (manifest.version() != 1 || to_version < 2) {
    return true;
  }

  if (manifest.has_direct()) {
    // Expect that the entry will be removed.
    return false;
  }

  manifest.set_version(2);
  return true;
}

}  // namespace

bool FileCache::Migrate(string::Hash hash, ui32 to_version) const {
  DCHECK(to_version <= kManifestVersion);

  const auto common_prefix = CommonPath(hash);
  const auto manifest_path = AppendExtension(common_prefix, base::kExtManifest);

  SQLite::Value entry;
  if (entries_ && entries_->Get(hash.str, &entry) &&
      std::get<SQLite::VERSION>(entry) == kManifestVersion) {
    LOG(CACHE_VERBOSE) << "No migration for " << manifest_path
                       << " required according to index";
    return true;
  }

  proto::Manifest manifest;
  bool modified = false;
  if (!base::LoadFromFile(manifest_path, &manifest)) {
    LOG(CACHE_ERROR) << "Failed to load " << manifest_path;
    return false;
  }

#define MIGRATE(from, to)                                            \
  if (!Version_##from##_to_##to(common_prefix, to_version, manifest, \
                                modified)) {                         \
    LOG(CACHE_ERROR) << "Failed to migrate " << manifest_path        \
                     << " from version " #from " to " #to;           \
    return false;                                                    \
  } else {                                                           \
    LOG(CACHE_VERBOSE) << "Migrated " << manifest_path               \
                       << " from version " #from " to " #to;         \
  }

  MIGRATE(0, 1);
  MIGRATE(1, 2);

#undef MIGRATE

  if (manifest.version() != to_version) {
    LOG(CACHE_ERROR) << "Not an actual version " << manifest_path << ": "
                     << manifest.version() << " vs. " << to_version;
    return false;
  }

  if (!modified) {
    // TODO: make a unit-test that we don't rewrite manifest on disk.
    LOG(CACHE_VERBOSE) << "No modifications for " << manifest_path;
    return true;
  } else if (!base::SaveToFile(manifest_path, manifest)) {
    LOG(CACHE_ERROR) << "Failed to save " << manifest_path;
    return false;
  }
  return true;
}

}  // namespace cache
}  // namespace dist_clang

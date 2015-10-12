#include <cache/migrator.h>

#include <base/file/file.h>
#include <base/logging.h>
#include <base/protobuf_utils.h>
#include <cache/manifest.pb.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

bool Version_0_to_1(const String& common_path, proto::Manifest& manifest,
                    bool& modified) {
  if (manifest.version() != 0) {
    return true;
  }
  manifest.clear_direct();
  manifest.clear_v1();

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
      const String err_path = common_path + ".stderr";
      if (base::File::Exists(err_path)) {
        size += base::File::Size(err_path);
      } else {
        return false;
      }
    }
    if (manifest.v1().obj()) {
      const String obj_path = common_path + ".o";
      if (base::File::Exists(obj_path)) {
        size += base::File::Size(obj_path);
      } else {
        return false;
      }
    }
    if (manifest.v1().dep()) {
      const String dep_path = common_path + ".d";
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

}  // namespace

bool Migrate(const String& common_path) {
  proto::Manifest manifest;
  bool modified = false;
  const String manifest_path = common_path + ".manifest";
  if (!base::LoadFromFile(manifest_path, &manifest)) {
    LOG(CACHE_ERROR) << "Failed to load " << manifest_path;
    return false;
  }

#define MIGRATE(from, to)                                           \
  if (!Version_##from##_to_##to(common_path, manifest, modified)) { \
    LOG(CACHE_ERROR) << "Failed to migrate " << manifest_path       \
                     << " from version " #from " to " #to;          \
    return false;                                                   \
  } else {                                                          \
    LOG(CACHE_VERBOSE) << "Migrated " << manifest_path              \
                       << " from version " #from " to " #to;        \
  }

  MIGRATE(0, 1);

#undef MIGRATE

  if (!modified) {
    // TODO: make an unit-test that we don't rewrite manifest on disk.
    return true;
  } else if (!base::SaveToFile(manifest_path, manifest)) {
    LOG(CACHE_ERROR) << "Failed to save " << manifest_path;
    return false;
  }
  return true;
}

}  // namespace cache
}  // namespace dist_clang

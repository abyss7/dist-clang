#include <cache/migrator.h>

#include <base/logging.h>
#include <base/protobuf_utils.h>
#include <cache/manifest.pb.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

bool Version_0_to_1(proto::Manifest& manifest) {
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

    v1->set_snappy(manifest.snappy());
    v1->set_err(manifest.stderr());
    v1->set_obj(manifest.object());
    v1->set_dep(manifest.deps());
  }

  manifest.clear_headers();
  manifest.clear_snappy();
  manifest.clear_stderr();
  manifest.clear_object();
  manifest.clear_deps();

  manifest.set_version(1);

  return true;
}

}  // namespace

bool Migrate(const String& manifest_path) {
  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest)) {
    LOG(CACHE_ERROR) << "Failed to load " << manifest_path;
    return false;
  }

#define MIGRATE(from, to)                                     \
  if (!Version_##from##_to_##to(manifest)) {                  \
    LOG(CACHE_ERROR) << "Failed to migrate " << manifest_path \
                     << " from version " #from " to " #to;    \
    return false;                                             \
  } else {                                                    \
    LOG(CACHE_VERBOSE) << "Migrated " << manifest_path        \
                       << " from version " #from " to " #to;  \
  }

  MIGRATE(0, 1);

#undef MIGRATE

  if (!base::SaveToFile(manifest_path, manifest)) {
    LOG(CACHE_ERROR) << "Failed to save " << manifest_path;
    return false;
  }
  return true;
}

}  // namespace cache
}  // namespace dist_clang

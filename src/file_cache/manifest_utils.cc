#include <file_cache/manifest_utils.h>

#include <file_cache/manifest.pb.h>

#include <third_party/protobuf/exported/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

#include <fcntl.h>

namespace dist_clang {
namespace file_cache {

bool LoadManifest(const String& path, proto::Manifest* manifest) {
  auto fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return false;
  }
  google::protobuf::io::FileInputStream input(fd);
  input.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&input, manifest)) {
    manifest->Clear();
    return false;
  }
  return true;
}

bool SaveManifest(const String& path, const proto::Manifest& manifest) {
  auto fd = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  if (fd == -1) {
    return false;
  }
  google::protobuf::io::FileOutputStream output(fd);
  output.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Print(manifest, &output)) {
    return false;
  }
  return true;
}

}  // namespace file_cache
}  // namespace dist_clang

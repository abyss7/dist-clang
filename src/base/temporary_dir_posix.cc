#include <base/temporary_dir.h>

#include <base/assert.h>
#include <base/c_utils.h>

namespace dist_clang {
namespace base {

TemporaryDir::TemporaryDir()
    : path_([this] {
        // FIXME: use |fs::temp_directory_path()|
        char buf[] = "/tmp/clangd-XXXXXX";
        if (!mkdtemp(buf)) {
          GetLastError(&error_);
          return Path();
        }
        return Path(buf);
      }()) {}

TemporaryDir::~TemporaryDir() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  DCHECK(!ec);
}

}  // namespace base
}  // namespace dist_clang

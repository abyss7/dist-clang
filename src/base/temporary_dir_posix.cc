#include <base/temporary_dir.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include <stdlib.h>

namespace dist_clang {
namespace base {

TemporaryDir::TemporaryDir()
    : path_([this] {
        // The directory from TMPDIR - e.g. on another file system for the
        // tests. Without the symlinks: the tests compare the paths, that the
        // code resolves.
        const char* dir = getenv("TMPDIR");
        std::error_code ec;
        const Path real_dir =
            std::filesystem::canonical(dir && *dir ? dir : "/tmp", ec);
        if (ec) {
          error_ = ec.message();
          return Path();
        }
        String buf = (real_dir / "clangd-XXXXXX").string();
        if (!mkdtemp(buf.data())) {
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

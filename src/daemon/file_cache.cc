#include "daemon/file_cache.h"

#include "base/file_utils.h"
#include "base/hash.h"
#include "base/logging.h"
#include "base/string_utils.h"

#include <fstream>
#include <sys/types.h>
#include <utime.h>

#include "base/using_log.h"

namespace dist_clang {
namespace daemon {

FileCache::FileCache(const std::string &path, uint64_t size_mb)
  : path_(path), size_mb_(size_mb) {
  pool_.Run();
}

bool FileCache::Find(const std::string& code, const std::string& command_line,
                     const std::string& version, Entry* entry) const {
  bool result = false;
  const std::string code_hash = base::Hexify(base::MakeHash(code));
  const std::string args_hash = base::Hexify(base::MakeHash(command_line));
  const std::string version_hash = base::Hexify(base::MakeHash(version));
  const std::string code_path = path_ + "/" + code_hash;
  const std::string args_path = code_path + "/" + args_hash;
  const std::string path = args_path + "/" + version_hash;

  std::ifstream obj(path + "/object");
  if (obj.is_open()) {
    DCHECK_O_EVAL(utime((path + "/object").c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(code_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = true;
    entry->first = path + "/object";
  }

  if (base::FileExists(path + "/stderr")) {
    DCHECK_O_EVAL(utime((path + "/stderr").c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(code_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = base::ReadFile(path + "/stderr", &entry->second);
  }

  return result;
}

void FileCache::Store(const std::string &code, const std::string &command_line,
                      const std::string &version, const Entry &entry) {
  std::string code_hash = base::Hexify(base::MakeHash(code));
  std::string args_hash = base::Hexify(base::MakeHash(command_line));
  std::string version_hash = base::Hexify(base::MakeHash(version));
  std::string path =
      path_ + "/" + code_hash + "/" + args_hash + "/" + version_hash;

  pool_.Push(std::bind(&FileCache::DoStore, this, path, entry));
}

void FileCache::DoStore(const std::string &path, const Entry &entry) {
  if (system((std::string("mkdir -p ") + path).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_VERBOSE) << "Failed to `mkdir -p`";
    return;
  }

  if (!base::CopyFile(entry.first, path + "/object.tmp")) {
    LOG(CACHE_VERBOSE) << "Failed to copy file to object.tmp";
    return;
  }
  if (!base::WriteFile(path + "/stderr", entry.second)) {
    base::DeleteFile(path + "/object.tmp");
    LOG(CACHE_VERBOSE) << "Failed to write stderr to file";
    return;
  }
  if (!base::MoveFile(path + "/object.tmp", path + "/object")) {
    base::DeleteFile(path + "/object.tmp");
    base::DeleteFile(path + "/stderr");
    LOG(CACHE_VERBOSE) << "Failed to move object.tmp to object";
    return;
  }

  if (size_mb_ != UNLIMITED) {
    // TODO: clean up cache, if it exceeds size limits.
    // The most ancient files should be wiped out.
  }
}

}  // namespace daemon
}  // namespace dist_clang

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
  const std::string version_hash = base::Hexify(base::MakeHash(version));
  const std::string args_hash = base::Hexify(base::MakeHash(command_line));
  const std::string code_hash = base::Hexify(base::MakeHash(code));

  const std::string version_path = path_ + "/" + version_hash;
  const std::string args_path = version_path + "/" + args_hash;
  const std::string object_path = args_path + "/" + code_hash + "-object";
  const std::string stderr_path = args_path + "/" + code_hash + "-stderr";

  std::ifstream obj(object_path);
  if (obj.is_open()) {
    DCHECK_O_EVAL(utime(object_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(version_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = true;
    entry->first = object_path;
  }

  if (base::FileExists(stderr_path)) {
    DCHECK_O_EVAL(utime(stderr_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(version_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = base::ReadFile(stderr_path, &entry->second);
  }

  return result;
}

void FileCache::Store(const std::string &code, const std::string &command_line,
                      const std::string &version, const Entry &entry) {
  std::string version_hash = base::Hexify(base::MakeHash(version));
  std::string args_hash = base::Hexify(base::MakeHash(command_line));
  std::string code_hash = base::Hexify(base::MakeHash(code));
  std::string path =
      path_ + "/" + version_hash + "/" + args_hash;

  pool_.Push(std::bind(&FileCache::DoStore, this, path, code_hash, entry));
}

void FileCache::DoStore(const std::string &path, const std::string& code_hash,
                        const Entry &entry) {
  if (system((std::string("mkdir -p ") + path).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << path;
    return;
  }

  const std::string object_path = path + "/" + code_hash + "-object";
  const std::string stderr_path = path + "/" + code_hash + "-stderr";

  if (!base::CopyFile(entry.first, object_path + ".tmp")) {
    LOG(CACHE_ERROR) << "Failed to copy " << entry.first << " to object.tmp";
    return;
  }
  if (!base::WriteFile(stderr_path, entry.second)) {
    base::DeleteFile(object_path + ".tmp");
    LOG(CACHE_ERROR) << "Failed to write stderr to file";
    return;
  }
  if (!base::MoveFile(object_path + ".tmp", object_path)) {
    base::DeleteFile(object_path + ".tmp");
    base::DeleteFile(stderr_path);
    LOG(CACHE_ERROR) << "Failed to move object.tmp to object";
    return;
  }

  LOG(CACHE_VERBOSE) << "File " << entry.first << " is cached on path " << path;

  if (size_mb_ != UNLIMITED) {
    // TODO: clean up cache, if it exceeds size limits.
    // The most ancient files should be wiped out.
  }
}

}  // namespace daemon
}  // namespace dist_clang

#include "daemon/file_cache.h"

#include "base/file_utils.h"
#include "base/hash.h"
#include "base/string_utils.h"

#include <fstream>

namespace dist_clang {
namespace daemon {

FileCache::FileCache(const std::string &path)
  : path_(path) {
  pool_.Run();
}

bool FileCache::Find(const std::string &code, const std::string &command_line,
                     const std::string &version, Entry *entry) const {
  bool result = false;
  std::string code_hash = base::Hexify(base::MakeHash(code));
  std::string args_hash = base::Hexify(base::MakeHash(command_line));
  std::string version_hash = base::Hexify(base::MakeHash(version));
  std::string path =
      path_ + "/" + code_hash + "/" + args_hash + "/" + version_hash;

  std::ifstream obj(path + "/object");
  if (obj.is_open()) {
    if (!entry)
      return true;
    result = true;
    entry->first = path + "/object";
  }

  std::ifstream stderr(path + "/stderr");
  if (stderr.is_open()) {
    if (!entry)
      return true;
    result = true;
    stderr >> entry->second;
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
  if (system((std::string("mkdir -p ") + path).c_str()) == -1)
    // "mkdir -p" doesn't fail if the path already exists.
    return;

  if (!base::CopyFile(entry.first, path + "/object.tmp"))
    return;
  std::ofstream stderr_file(path + "/stderr", std::ios::out|std::ios::trunc);
  if (!stderr_file.is_open()) {
    base::DeleteFile(path + "/object.tmp");
    return;
  }
  stderr_file << entry.second;
  stderr_file.close();
  if (!base::MoveFile(path + "/object.tmp", path + "/object")) {
    base::DeleteFile(path + "/object.tmp");
    base::DeleteFile(path + "/stderr");
  }
}

}  // namespace daemon
}  // namespace dist_clang

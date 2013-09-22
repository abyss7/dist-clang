#pragma once

#include "daemon/thread_pool.h"

#include <string>

namespace dist_clang {
namespace daemon {

class FileCache {
  public:
    typedef std::pair<std::string /* path */, std::string /* stderr */> Entry;

    explicit FileCache(const std::string& path);

    bool Find(const std::string& code, const std::string& command_line,
              const std::string& version, Entry* entry) const;
    void Store(const std::string& code, const std::string& command_line,
               const std::string& version, const Entry& entry);

  private:
    const std::string path_;
    ThreadPool pool_;

    void DoStore(const std::string& path, const Entry& entry);
};

}  // namespace daemon
}  // namespace dist_clang

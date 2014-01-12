#pragma once

#include <string>

namespace dist_clang {
namespace base {

class TemporaryDir {
  public:
    TemporaryDir();
    ~TemporaryDir();

    inline const std::string& GetPath() const;
    inline const std::string& GetError() const;

    operator std::string() const;

  private:
    std::string path_, error_;
};

const std::string& TemporaryDir::GetPath() const {
  return path_;
}

const std::string& TemporaryDir::GetError() const {
  return error_;
}

}  // namespace base
}  // namespace dist_clang

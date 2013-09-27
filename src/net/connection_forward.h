#pragma once

#include <memory>

namespace dist_clang {
namespace net {

class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
using ConnectionWeakPtr = std::weak_ptr<Connection>;

}  // namespace net
}  // namespace dist_clang

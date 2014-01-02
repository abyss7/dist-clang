#pragma once

#include <memory>

namespace dist_clang {
namespace net {

class Connection;
class ConnectionImpl;
using ConnectionPtr = std::shared_ptr<ConnectionImpl>;
using ConnectionWeakPtr = std::weak_ptr<ConnectionImpl>;

class EndPoint;
using EndPointPtr = std::shared_ptr<EndPoint>;

}  // namespace net
}  // namespace dist_clang

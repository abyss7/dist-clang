#pragma once

#include <memory>

namespace dist_clang {
namespace net {

class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
using ConnectionWeakPtr = std::weak_ptr<Connection>;

class ConnectionImpl;
using ConnectionImplPtr = std::shared_ptr<ConnectionImpl>;

class EndPoint;
using EndPointPtr = std::shared_ptr<EndPoint>;

}  // namespace net
}  // namespace dist_clang

#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace net {

class Connection;
using ConnectionPtr = SharedPtr<Connection>;
using ConnectionWeakPtr = WeakPtr<Connection>;

class ConnectionImpl;
using ConnectionImplPtr = SharedPtr<ConnectionImpl>;

class EndPoint;
using EndPointPtr = SharedPtr<EndPoint>;

class TestConnection;
using TestConnectionPtr = SharedPtr<TestConnection>;

}  // namespace net
}  // namespace dist_clang

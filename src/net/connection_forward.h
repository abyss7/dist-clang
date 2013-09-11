#pragma once

#include <memory>

namespace dist_clang {
namespace net {

class Connection;
typedef std::shared_ptr<Connection> ConnectionPtr;
typedef std::weak_ptr<Connection> ConnectionWeakPtr;

}
}

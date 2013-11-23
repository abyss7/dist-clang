#pragma once

#include "base/attributes.h"
#include "base/testable.h"
#include "net/base/types.h"

#include <functional>
#include <memory>
#include <string>

namespace dist_clang {
namespace net {

class NetworkServiceImpl;

class NetworkService:
    public base::Testable<NetworkService, NetworkServiceImpl> {
  public:
    using ListenCallback = std::function<void(ConnectionPtr)>;

    virtual ~NetworkService() {}

    virtual bool Run() THREAD_UNSAFE = 0;

    virtual bool Listen(
        const std::string& path,
        ListenCallback callback,
        std::string* error = nullptr) THREAD_UNSAFE = 0;
    virtual bool Listen(
        const std::string& host,
        unsigned short port,
        ListenCallback callback,
        std::string* error = nullptr) THREAD_UNSAFE = 0;

    virtual ConnectionPtr Connect(
        const std::string& path,
        std::string* error = nullptr) THREAD_SAFE = 0;
    virtual ConnectionPtr Connect(
        EndPointPtr end_point,
        std::string* error = nullptr) THREAD_SAFE = 0;
};

}  // namespace net
}  // namespace dist_clang

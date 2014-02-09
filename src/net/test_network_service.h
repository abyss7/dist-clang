#pragma once

#include "base/empty_lambda.h"
#include "base/hash.h"
#include "net/base/end_point.h"
#include "net/network_service.h"
#include "net/test_connection.h"

#include <unordered_map>

namespace std {

template <class T, class U>
struct hash<pair<T, U>> {
  public:
    size_t operator() (const pair<T, U>& value) const {
      size_t seed = 0;
      dist_clang::base::HashCombine(seed, value.first);
      dist_clang::base::HashCombine(seed, value.second);
      return seed;
    }
};

}  // namespace std

namespace dist_clang {
namespace net {

class TestNetworkService: public NetworkService {
  public:
    using TestConnectionPtr = std::shared_ptr<TestConnection>;
    using OnConnectCallback =
        std::function<TestConnectionPtr(EndPointPtr, std::string*)>;
    using OnListenCallback =
        std::function<bool(const std::string&, unsigned short, std::string*)>;

    class Factory: public NetworkService::Factory {
      public:
        using OnCreateCallback = std::function<void(TestNetworkService*)>;

        virtual std::unique_ptr<NetworkService> Create() override;

        inline void CallOnCreate(OnCreateCallback callback);

      private:
        OnCreateCallback on_create_ = EmptyLambda<>();
    };

    inline virtual bool Run() override;

    virtual bool Listen(
        const std::string& path,
        ListenCallback callback,
        std::string* error) override;

    virtual bool Listen(
        const std::string& host,
        unsigned short port,
        ListenCallback callback,
        std::string* error) override;

    virtual ConnectionPtr Connect(
        EndPointPtr end_point,
        std::string* error) override;

    ConnectionPtr TriggerListen(const std::string& host, uint16_t port = 0);

    inline void CallOnConnect(OnConnectCallback callback);
    inline void CallOnListen(OnListenCallback callback);
    inline void CountConnectAttempts(uint* counter);
    inline void CountListenAttempts(uint* counter);

  private:
    using HostPortPair = std::pair<std::string, unsigned short>;

    OnConnectCallback on_connect_ = EmptyLambda<TestConnectionPtr>();
    OnListenCallback on_listen_ = EmptyLambda<bool>(false);
    uint* connect_attempts_ = nullptr;
    uint* listen_attempts_ = nullptr;
    std::unordered_map<HostPortPair, ListenCallback> listen_callbacks_;
};

void TestNetworkService::Factory::CallOnCreate(OnCreateCallback callback) {
  on_create_ = callback;
}

bool TestNetworkService::Run() {
  return true;
}

void TestNetworkService::CallOnConnect(OnConnectCallback callback) {
  on_connect_ = callback;
}

void TestNetworkService::CallOnListen(OnListenCallback callback) {
  on_listen_ = callback;
}

void TestNetworkService::CountConnectAttempts(uint* counter) {
  connect_attempts_ = counter;
}

void TestNetworkService::CountListenAttempts(uint* counter) {
  listen_attempts_ = counter;
}

}  // namespace net
}  // namespace dist_clang

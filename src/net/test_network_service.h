#pragma once

#include <base/empty_lambda.h>
#include <base/hash.h>
#include <net/network_service.h>

namespace std {

template <class T, class U>
struct hash<pair<T, U>> {
 public:
  size_t operator()(const pair<T, U>& value) const {
    size_t seed = 0;
    dist_clang::base::HashCombine(seed, value.first);
    dist_clang::base::HashCombine(seed, value.second);
    return seed;
  }
};

}  // namespace std

namespace dist_clang {
namespace net {

class TestNetworkService : public NetworkService {
 public:
  using OnConnectCallback = Fn<TestConnectionPtr(EndPointPtr, String*)>;
  using OnListenCallback = Fn<bool(const String&, ui16, String*)>;

  class Factory : public NetworkService::Factory {
   public:
    using OnCreateCallback = Fn<void(TestNetworkService*)>;

    virtual UniquePtr<NetworkService> Create() override;

    inline void CallOnCreate(OnCreateCallback callback);

   private:
    OnCreateCallback on_create_ = EmptyLambda<>();
  };

  inline virtual bool Run() override;

  virtual bool Listen(const String& path, ListenCallback callback,
                      String* error) override;

  virtual bool Listen(const String& host, ui16 port, ListenCallback callback,
                      String* error) override;

  virtual ConnectionPtr Connect(EndPointPtr end_point, String* error) override;

  ConnectionPtr TriggerListen(const String& host, ui16 port = 0);

  inline void CallOnConnect(OnConnectCallback callback);
  inline void CallOnListen(OnListenCallback callback);
  inline void CountConnectAttempts(ui32* counter);
  inline void CountListenAttempts(ui32* counter);

 private:
  using HostPortPair = Pair<String, ui16>;

  OnConnectCallback on_connect_ = EmptyLambda<TestConnectionPtr>();
  OnListenCallback on_listen_ = EmptyLambda<bool>(false);
  ui32* connect_attempts_ = nullptr;
  ui32* listen_attempts_ = nullptr;
  HashMap<HostPortPair, ListenCallback> listen_callbacks_;
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

void TestNetworkService::CountConnectAttempts(ui32* counter) {
  connect_attempts_ = counter;
}

void TestNetworkService::CountListenAttempts(ui32* counter) {
  listen_attempts_ = counter;
}

}  // namespace net
}  // namespace dist_clang

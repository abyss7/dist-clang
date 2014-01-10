#pragma once

#include "net/network_service.h"
#include "net/test_connection.h"

namespace dist_clang {
namespace net {

template <bool DoConnect>
class TestNetworkService: public NetworkService {
  public:
    using TestConnectionPtr = std::shared_ptr<TestConnection>;
    using This = TestNetworkService<DoConnect>;

    class Factory: public NetworkService::Factory {
      public:
        Factory() : on_create_([](This*) {}) {}

        virtual std::unique_ptr<NetworkService> Create() override {
          auto new_t = new This;
          on_create_(new_t);
          return std::unique_ptr<NetworkService>(new_t);
        }

        void CallOnCreate(std::function<void(This*)> callback) {
          on_create_ = callback;
        }

      private:
        std::function<void(This*)> on_create_;
    };

    TestNetworkService()
      : on_connect_([](TestConnection*) {}),
        connect_attempts_(nullptr) {}

    virtual bool Run() override {
      return false;
    }

    virtual bool Listen(
        const std::string& path,
        ListenCallback callback,
        std::string* error) override {
      return false;
    }

    virtual bool Listen(
        const std::string& host,
        unsigned short port,
        ListenCallback callback,
        std::string* error) override {
      return false;
    }

    virtual net::ConnectionPtr Connect(
        net::EndPointPtr end_point,
        std::string* error) override {
      if (connect_attempts_) {
        (*connect_attempts_)++;
      }

      if (!DoConnect) {
        if (error) {
          error->assign("Test service rejects connection intentionally");
        }
        return net::ConnectionPtr();
      }
      else {
        auto new_connection = TestConnectionPtr(new TestConnection);
        on_connect_(new_connection.get());
        return new_connection;
      }
    }

    void CallOnConnect(std::function<void(TestConnection*)> callback) {
      on_connect_ = callback;
    }

    void CountConnectAttempts(uint* counter) {
      connect_attempts_ = counter;
    }

  private:
    std::function<void(TestConnection*)> on_connect_;
    uint* connect_attempts_;
};

}  // namespace net
}  // namespace dist_clang

#pragma once

#include <base/process_impl.h>
#include <base/test_process.h>
#include <daemon/configuration.pb.h>
#include <net/network_service_impl.h>
#include <net/test_end_point_resolver.h>
#include <net/test_network_service.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace daemon {

struct CommonDaemonTest : public ::testing::Test {
  using Service = net::TestNetworkService;
  using ListenCallback = Fn<bool(const String&, ui16, String*)>;
  using ConnectCallback = Fn<void(net::TestConnection*)>;
  using RunCallback = Fn<void(base::TestProcess*)>;

  virtual void SetUp() override {
    {
      auto factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        ASSERT_EQ(nullptr, test_service);
        test_service = service;
        service->CountConnectAttempts(&connect_count);
        service->CountListenAttempts(&listen_count);
        service->CallOnConnect([this](net::EndPointPtr, String*) {
          auto connection = Service::TestConnectionPtr(new net::TestConnection);
          connection->CountSendAttempts(&send_count);
          connection->CountReadAttempts(&read_count);
          ++connections_created;
          connect_callback(connection.get());

          return connection;
        });
        service->CallOnListen(listen_callback);
      });
    }

    {
      auto factory = base::Process::SetFactory<base::TestProcess::Factory>();
      factory->CallOnCreate([this](base::TestProcess* process) {
        process->CountRuns(&run_count);
        process->CallOnRun([this, process](ui32, const String&, String* error) {
          run_callback(process);

          if (!do_run) {
            if (error) {
              error->assign("Test process fails to run intentionally");
            }
            return false;
          }

          return true;
        });
      });
    }

    net::EndPointResolver::SetFactory<net::TestEndPointResolver::Factory>();
  }

  virtual void TearDown() override {
    net::NetworkService::SetFactory<Service::Factory>();
    base::Process::SetFactory<base::TestProcess::Factory>();
    net::EndPointResolver::SetFactory<net::TestEndPointResolver::Factory>();
  }

  ListenCallback listen_callback = EmptyLambda<bool>(true);
  ConnectCallback connect_callback = EmptyLambda<>();
  RunCallback run_callback = EmptyLambda<>();

  Service* WEAK_PTR test_service = nullptr;
  proto::Configuration conf;
  bool do_run = true;

  std::mutex send_mutex;
  std::condition_variable send_condition;

  ui32 listen_count = 0, connect_count = 0, read_count = 0, send_count = 0,
       run_count = 0, connections_created = 0;
};

}  // namespace daemon
}  // namespace dist_clang

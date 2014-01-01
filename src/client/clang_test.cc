#include "base/c_utils.h"
#include "base/constants.h"
#include "base/file_utils.h"
#include "client/clang.h"
#include "client/clang_flag_set.h"
#include "gtest/gtest.h"
#include "net/network_service_impl.h"
#include "proto/remote.pb.h"

namespace dist_clang {
namespace client {

TEST(ClientTest, ClangFlagSetTest) {
  std::list<std::string> input = {
    "",
    "/home/test/.local/bin/clang", "-cc1",
    "-triple", "x86_64-unknown-linux-gnu",
    "-emit-obj",
    "-mrelax-all",
    "-disable-free",
    "-main-file-name", "test.cc",
    "-mrelocation-model", "static",
    "-mdisable-fp-elim",
    "-fmath-errno",
    "-masm-verbose",
    "-mconstructor-aliases",
    "-munwind-tables",
    "-fuse-init-array",
    "-target-cpu", "x86-64",
    "-target-linker-version", "2.23.2",
    "-coverage-file", "/tmp/test.o",
    "-resource-dir", "/home/test/.local/lib/clang/3.4",
    "-internal-isystem", "/usr/include/c++/4.8.2",
    "-internal-isystem", "/usr/include/c++/4.8.2/x86_64-redhat-linux",
    "-internal-isystem", "/usr/include/c++/4.8.2/backward",
    "-internal-isystem", "/usr/include/x86_64-redhat-linux/c++/4.8.2",
    "-internal-isystem", "/usr/local/include",
    "-internal-isystem", "/home/test/.local/lib/clang/3.4/include",
    "-internal-externc-isystem", "/include",
    "-internal-externc-isystem", "/usr/include",
    "-fdeprecated-macro",
    "-fdebug-compilation-dir", "/tmp",
    "-ferror-limit", "19",
    "-fmessage-length", "213",
    "-mstackrealign",
    "-fobjc-runtime=gcc",
    "-fcxx-exceptions",
    "-fexceptions",
    "-fdiagnostics-show-option",
    "-fcolor-diagnostics",
    "-vectorize-slp",
    "-o", "test.o",
    "-x", "c++",
    "test.cc"
  };

  proto::Flags expected_flags;
  expected_flags.mutable_compiler()->set_path("/home/test/.local/bin/clang");
  expected_flags.mutable_compiler()->set_version("0.0");
  expected_flags.set_output("test.o");
  expected_flags.set_input("test.cc");
  expected_flags.set_language("c++");
  expected_flags.add_other()->assign("-cc1");
  expected_flags.add_other()->assign("-triple");
  expected_flags.add_other()->assign("x86_64-unknown-linux-gnu");
  expected_flags.add_other()->assign("-emit-obj");
  expected_flags.add_other()->assign("-mrelax-all");
  expected_flags.add_other()->assign("-disable-free");
  expected_flags.add_other()->assign("-mrelocation-model");
  expected_flags.add_other()->assign("static");
  expected_flags.add_other()->assign("-mdisable-fp-elim");
  expected_flags.add_other()->assign("-fmath-errno");
  expected_flags.add_other()->assign("-masm-verbose");
  expected_flags.add_other()->assign("-mconstructor-aliases");
  expected_flags.add_other()->assign("-munwind-tables");
  expected_flags.add_other()->assign("-fuse-init-array");
  expected_flags.add_other()->assign("-target-cpu");
  expected_flags.add_other()->assign("x86-64");
  expected_flags.add_other()->assign("-target-linker-version");
  expected_flags.add_other()->assign("2.23.2");
  expected_flags.add_other()->assign("-fdeprecated-macro");
  expected_flags.add_other()->assign("-ferror-limit");
  expected_flags.add_other()->assign("19");
  expected_flags.add_other()->assign("-fmessage-length");
  expected_flags.add_other()->assign("213");
  expected_flags.add_other()->assign("-mstackrealign");
  expected_flags.add_other()->assign("-fobjc-runtime=gcc");
  expected_flags.add_other()->assign("-fcxx-exceptions");
  expected_flags.add_other()->assign("-fexceptions");
  expected_flags.add_other()->assign("-fdiagnostics-show-option");
  expected_flags.add_other()->assign("-fcolor-diagnostics");
  expected_flags.add_other()->assign("-vectorize-slp");
  expected_flags.add_non_cached()->assign("-main-file-name");
  expected_flags.add_non_cached()->assign("test.cc");
  expected_flags.add_non_cached()->assign("-coverage-file");
  expected_flags.add_non_cached()->assign("/tmp/test.o");
  expected_flags.add_non_cached()->assign("-resource-dir");
  expected_flags.add_non_cached()->assign("/home/test/.local/lib/clang/3.4");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/include/c++/4.8.2");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/usr/include/c++/4.8.2/x86_64-redhat-linux");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/include/c++/4.8.2/backward");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/usr/include/x86_64-redhat-linux/c++/4.8.2");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/local/include");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/home/test/.local/lib/clang/3.4/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/usr/include");
  expected_flags.add_non_cached()->assign("-fdebug-compilation-dir");
  expected_flags.add_non_cached()->assign("/tmp");

  proto::Flags actual_flags;
  actual_flags.mutable_compiler()->set_version("0.0");
  ASSERT_EQ(ClangFlagSet::COMPILE,
            ClangFlagSet::ProcessFlags(input, &actual_flags));
  ASSERT_EQ(expected_flags.SerializeAsString(),
            actual_flags.SerializeAsString());
}

namespace {

template <class T>
class TestFactory: public net::NetworkService::Factory {
  public:
    TestFactory() : on_create_([](T*) {}) {}

    virtual std::unique_ptr<net::NetworkService> Create() override {
      auto new_t = new T;
      on_create_(new_t);
      return std::unique_ptr<net::NetworkService>(new_t);
    }

    void CallOnCreate(std::function<void(T*)> callback) {
      on_create_ = callback;
    }

  private:
    std::function<void(T*)> on_create_;
};

template <bool DoConnect>
class TestService: public net::NetworkService {
  public:
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
      if (!DoConnect) {
        if (error) {
          error->assign("Test service rejects connection intentionally");
        }
        return net::ConnectionPtr();
      }
      else {
        // TODO: implement this.
        return net::ConnectionPtr();
      }
    }
};

}  // namespace

TEST(ClientTest, NoConnection) {
  net::NetworkService::SetFactory<TestFactory<TestService<false>>>();

  int argc = 3;
  const char* argv[] = {"a", "b", "c", nullptr};

  ASSERT_TRUE(client::DoMain(argc, argv));
}

TEST(ClientTest, DISABLED_NoEnvironmentVariable) {
  // TODO: implement this.

  net::NetworkService::SetFactory<TestFactory<TestService<true>>>();
}

TEST(ClientTest, DISABLED_SuccessfulCompilation) {
  // TODO: implement this.
  using Service = TestService<true>;

  auto factory = net::NetworkService::SetFactory<TestFactory<Service>>();
  factory->CallOnCreate([](Service*) {});

  // Expect one connection on specific socket path.
  // Expect sending one message.
  //   Message type should be Execute.
  //   remote = false
  //   Message should contain some specific cc and pp flags among others.
  //   Message should have specific current_dir
  // Expect reading one message.
}

/*
 *
 * No CLANGD_CXX environment variable.
 * No input file.
 * Can't send message.
 * Can't read message.
 * Daemon sends malformed message.
 * Successful compilation.
 * Failed compilation.
 *
 */

}  // namespace client
}  // namespace dist_clang

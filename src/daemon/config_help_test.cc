#include <daemon/config_help.h>

#include <base/types.h>
#include <daemon/configuration.pb.h>
#include <google/protobuf/descriptor.pb.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace daemon {

namespace {

// If the |name| is shown as a field: "name: ..." or "name { ... }".
bool HasField(const String& help, const String& name) {
  for (size_t pos = help.find(name); pos != String::npos;
       pos = help.find(name, pos + 1)) {
    const bool starts = pos == 0 || help[pos - 1] == ' ' || help[pos - 1] == '\n';
    const String rest = help.substr(pos + name.size(), 2);
    if (starts && (rest.starts_with(":") || rest == " {")) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(ConfigHelpTest, AllFields) {
  using google::protobuf::Descriptor;
  using google::protobuf::FieldDescriptor;
  const String help = kConfigHelp;

  // Every field of the configuration and of its parts is described - with
  // the values of the enums.
  HashSet<const Descriptor*> checked;
  Fn<void(const Descriptor*)> Check = [&](const Descriptor* message) {
    if (!checked.insert(message).second) {
      return;
    }
    for (int i = 0; i < message->field_count(); ++i) {
      const FieldDescriptor* field = message->field(i);
      // The obsolete fields and the ones, that aren't implemented, are
      // hidden.
      if (field->options().deprecated()) {
        EXPECT_FALSE(HasField(help, String(field->name())))
            << message->full_name() << "." << field->name();
        continue;
      }
      EXPECT_TRUE(HasField(help, String(field->name())))
          << message->full_name() << "." << field->name();
      if (field->message_type()) {
        Check(field->message_type());
      }
      if (field->enum_type()) {
        for (int j = 0; j < field->enum_type()->value_count(); ++j) {
          const auto* value = field->enum_type()->value(j);
          const String name(value->name());
          EXPECT_EQ(!value->options().deprecated(),
                    help.find(name) != String::npos)
              << field->full_name() << " = " << name;
        }
      }
    }
  };
  Check(proto::Configuration::descriptor());

  // The fields of the local daemon go first - the cache at the top - and the
  // ones of the distributed compilation after them.
  const size_t cache = help.find("\ncache {");
  const size_t emitter = help.find("\nemitter {");
  const size_t distributed = help.find("The distributed compilation");
  const size_t absorber = help.find("\nabsorber {");
  const size_t remotes = help.find("  remotes {");
  EXPECT_LT(cache, emitter);
  EXPECT_LT(emitter, distributed);
  EXPECT_NE(String::npos, distributed);
  EXPECT_LT(distributed, absorber);
  EXPECT_LT(distributed, remotes);
  EXPECT_NE(String::npos, absorber);
  EXPECT_NE(String::npos, remotes);

  // The types, the defaults and the comments.
  EXPECT_NE(String::npos, help.find("clean_period: uint32 = 600")) << help;
  EXPECT_NE(String::npos, help.find("Interval in seconds for polling"));
}

}  // namespace daemon
}  // namespace dist_clang

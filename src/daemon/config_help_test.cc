#include <daemon/config_help.h>

#include <base/types.h>
#include <daemon/configuration.pb.h>

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
      EXPECT_TRUE(HasField(help, String(field->name())))
          << message->full_name() << "." << field->name();
      if (field->message_type()) {
        Check(field->message_type());
      }
      if (field->enum_type()) {
        for (int j = 0; j < field->enum_type()->value_count(); ++j) {
          const String value(field->enum_type()->value(j)->name());
          EXPECT_NE(String::npos, help.find(value))
              << field->full_name() << " = " << value;
        }
      }
    }
  };
  Check(proto::Configuration::descriptor());

  // The types, the defaults and the comments.
  EXPECT_NE(String::npos, help.find("clean_period: uint32 = 600")) << help;
  EXPECT_NE(String::npos, help.find("Interval in seconds for polling"));
}

}  // namespace daemon
}  // namespace dist_clang

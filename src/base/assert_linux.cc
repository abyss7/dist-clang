#include <base/assert.h>

#include <base/string_utils.h>

#include <third_party/libcxxabi/exported/include/cxxabi.h>

#include <execinfo.h>

namespace dist_clang {

namespace {

String Demangle(const char* backtrace_symbol) {
  const String string = backtrace_symbol;

  auto begin_name = string.find('(');
  if (begin_name == String::npos) {
    return string;
  }
  begin_name++;

  const auto end_name = string.find('+', begin_name);
  if (end_name == String::npos) {
    return string;
  }

  const String mangled_name = string.substr(begin_name, end_name - begin_name);
  size_t size = 256;
  int status;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name.c_str(), nullptr, &size, &status);
  if (status == 0) {
    auto result = String(demangled_name);
    free(demangled_name);
    return result;
  } else {
    if (demangled_name) {
      free(demangled_name);
    }
    return mangled_name;
  }
}

}  // namespace

namespace base {

void GetStackTrace(ui8 depth, Vector<String>& strings) {
  using void_ptr = void*;
  UniquePtr<void_ptr[]> buffer(new void_ptr[depth + 1]);

  auto size = backtrace(buffer.get(), depth + 1);
  auto symbols = backtrace_symbols(buffer.get(), size);
  strings.resize(size - 1);
  for (int i = 1; i < size; ++i) {
    strings[i - 1] = Demangle(symbols[i]);
    Replace(strings[i - 1],
            "std::__1::basic_string<char, std::__1::char_traits<char>, "
            "std::__1::allocator<char> >",
            "std::string");
  }
  free(symbols);
}

}  // namespace base
}  // namespace dist_clang

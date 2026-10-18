#include <base/path_utils.h>

namespace dist_clang {
namespace base {

String NormalPath(const String& dir, const String& path) {
  String result;
  result.reserve(dir.size() + path.size() + 1);
  // The positions of the separators before the components of the result - to
  // drop the last one on "..".
  Vector<size_t> starts;
  auto Append = [&](const String& part) {
    size_t start = 0;
    while (start <= part.size()) {
      size_t end = part.find('/', start);
      if (end == String::npos) {
        end = part.size();
      }
      const std::string_view name(part.data() + start, end - start);
      if (name == "..") {
        if (!starts.empty()) {
          result.resize(starts.back());
          starts.pop_back();
        }
      } else if (!name.empty() && name != ".") {
        starts.push_back(result.size());
        result.push_back('/');
        result.append(name);
      }
      start = end + 1;
    }
  };
  if (path.empty() || path[0] != '/') {
    Append(dir);
  }
  Append(path);
  return result.empty() ? String("/") : result;
}

}  // namespace base
}  // namespace dist_clang

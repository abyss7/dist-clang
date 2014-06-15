#pragma once

#include <base/aliases.h>

#include <third_party/libcxx/exported/include/iomanip>
#include <third_party/libcxx/exported/include/sstream>

namespace dist_clang {
namespace base {

template <char delimiter>
inline void SplitString(const String& input, List<String>& tokens) {
  size_t prev = 0;
  size_t i = input.find(delimiter);
  while (i != String::npos) {
    tokens.push_back(input.substr(prev, i - prev));
    prev = i + sizeof(delimiter);
    i = input.find(delimiter, prev);
  }
  tokens.push_back(input.substr(prev));
}

template <>
inline void SplitString<'\n'>(const String& input, List<String>& tokens) {
  std::istringstream ss(input);
  String line;
  while (std::getline(ss, line)) tokens.push_back(line);
}

inline void SplitString(const String& input, const String& delimiter,
                        List<String>& tokens) {
  size_t prev = 0;
  size_t i = input.find(delimiter);
  while (i != String::npos) {
    tokens.push_back(input.substr(prev, i - prev));
    prev = i + delimiter.size();
    i = input.find(delimiter, prev);
  }
  tokens.push_back(input.substr(prev));
}

template <char delimiter, class T>
inline String JoinString(const T& begin, const T& end) {
  String output;

  for (auto it = begin; it != end; ++it) {
    if (it != begin) output += delimiter;
    output += *it;
  }

  return output;
}

inline String Hexify(const String& binary) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0, s = binary.size(); i < s; ++i) {
    ss << std::setw(2) << static_cast<ui32>(static_cast<ui8>(binary[i]));
  }
  return ss.str();
}

inline void Replace(String& input, const String& replacee,
                    const String& replacer) {
  size_t pos = 0;
  while ((pos = input.find(replacee, pos)) != String::npos) {
    input.replace(pos, replacee.length(), replacer);
    pos += replacer.length();
  }
}

template <typename T>
inline T StringTo(const String& str) {
  T result;
  std::stringstream ss(str);
  ss >> result;
  return result;
}

}  // namespace base
}  // namespace dist_clang

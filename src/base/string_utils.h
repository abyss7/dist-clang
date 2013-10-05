#pragma once

#include <iomanip>
#include <list>
#include <sstream>
#include <string>

namespace dist_clang {
namespace base {

template<char delimiter>
inline void SplitString(const std::string& input,
                        std::list<std::string>& tokens) {
  size_t prev = 0;
  size_t i = input.find(delimiter);
  while (i != std::string::npos) {
    tokens.push_back(input.substr(prev, i - prev));
    prev = i + sizeof(delimiter);
    i = input.find(delimiter, prev);
  }
  tokens.push_back(input.substr(prev));
}

template<>
inline void SplitString<'\n'>(const std::string& input,
                              std::list<std::string>& tokens) {
  std::istringstream ss(input);
  std::string line;
  while(std::getline(ss, line))
    tokens.push_back(line);
}

inline void SplitString(const std::string &input, const std::string& delimiter,
                        std::list<std::string> &tokens) {
  size_t prev = 0;
  size_t i = input.find(delimiter);
  while (i != std::string::npos) {
    tokens.push_back(input.substr(prev, i - prev));
    prev = i + delimiter.size();
    i = input.find(delimiter, prev);
  }
  tokens.push_back(input.substr(prev));
}

template<char delimiter, class T>
inline std::string JoinString(const T& begin, const T& end) {
  std::string output;

  for (auto it = begin; it != end; ++it) {
    if (it != begin)
      output += delimiter;
    output += *it;
  }

  return output;
}

inline std::string Hexify(const std::string& binary) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0, s = binary.size(); i < s; ++i) {
    ss << std::setw(2)
       << static_cast<unsigned>(static_cast<unsigned char>(binary[i]));
  }
  return ss.str();
}

template <typename Int>
inline Int StringToInt(const std::string& str) {
  Int result;
  std::stringstream ss(str);
  ss >> result;
  return result;
}

template <typename Int>
inline std::string IntToString(Int integer) {
  std::string result;
  std::stringstream ss(result);
  ss << integer;
  return result;
}

inline void Replace(std::string& input, const std::string& replacee,
                    const std::string& replacer) {
  size_t pos = 0;
  while ((pos = input.find(replacee, pos)) != std::string::npos) {
     input.replace(pos, replacee.length(), replacer);
     pos += replacer.length();
  }
}

}  // namespace base
}  // namespace dist_clang

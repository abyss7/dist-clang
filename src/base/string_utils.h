#pragma once

#include <list>
#include <sstream>
#include <string>

namespace {

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
    prev = i + sizeof(delimiter);
    i = input.find(delimiter, prev);
  }
}

}  // namespace

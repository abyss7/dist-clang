#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace dist_clang {
namespace base {

class Chronometer {
  public:
    explicit Chronometer(const std::string& label);
    Chronometer(const std::string& label, Chronometer& parent);
    ~Chronometer();

  private:
    void AddInterval(const std::string& label, unsigned long time);

    const size_t MAX_INTERVALS = 10;

    const std::string label_;
    struct timespec start_time_;
    bool error_;
    Chronometer* parent_;
    std::vector<std::pair<std::string, unsigned long>> intervals_;
    std::atomic<size_t> interval_index_;
};

}  // namespace base
}  // namespace dist_clang

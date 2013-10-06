#include "base/chronometer.h"

#include "base/assert.h"
#include "base/string_utils.h"

#include <iostream>

#include <time.h>

namespace dist_clang {
namespace base {

Chronometer::Chronometer(const std::string &label)
  : label_(label), error_(false), parent_(nullptr), intervals_(MAX_INTERVALS),
    interval_index_(0) {
  if (clock_gettime(CLOCK_MONOTONIC, &start_time_) == -1) {
    error_ = true;
  }
}

Chronometer::Chronometer(const std::string &label, Chronometer& parent)
  : Chronometer(label) {
  base::Assert(!parent.parent_);
  parent_ = &parent;
}

Chronometer::~Chronometer() {
  struct timespec end_time;
  if (clock_gettime(CLOCK_MONOTONIC, &end_time) == -1) {
    error_ = true;
  }
  if (!error_) {
    unsigned long diff = (end_time.tv_nsec - start_time_.tv_nsec) / 1000000;
    diff += 1000 * (end_time.tv_sec - start_time_.tv_sec);
    if (parent_) {
      auto i = parent_->interval_index_.fetch_add(1);
      base::Assert(i < MAX_INTERVALS);
      parent_->intervals_[i].first = label_;
      parent_->intervals_[i].second = diff;
    }
    else {
      std::cout << label_ + " took " << diff << " ms" << std::endl;
      if (interval_index_) {
        for (size_t i = 0; i < interval_index_; ++i) {
          unsigned percent = intervals_[i].second * 100 / diff;
          std::cout << "  " << intervals_[i].first << " took "
                    << intervals_[i].second << " ms " << percent << "%"
                    << std::endl;
        }
      }
    }
  }
}

}  // namespace base
}  // namespace dist_clang

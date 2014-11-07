#include <base/const_string.h>

#include <base/assert.h>
#include <base/attributes.h>

namespace dist_clang {

namespace {
auto NoopDeleter = [](const char*) {};
auto CharArrayDeleter = std::default_delete<const char[]>();
}

namespace base {

ConstString::ConstString(Literal str)
    : str_(str.str_, NoopDeleter), size_(strlen(str.str_)), null_end_(true) {
  DCHECK(str.str_[size_] == '\0');
}

ConstString::ConstString(char str[], size_t size)
    : str_(str, CharArrayDeleter), size_(size) {
}

ConstString::ConstString(UniquePtr<char[]>& str, size_t size)
    : str_(str.release(), CharArrayDeleter), size_(size) {
}

ConstString::ConstString(String&& str)
    : medium_(new String(std::move(str))),
      str_(medium_->data(), NoopDeleter),
      size_(medium_->size()),
      null_end_(true) {
  DCHECK(medium_->data()[size_] == '\0');
}

ConstString::ConstString(Rope&& rope) : rope_(std::move(rope)) {
  for (const auto& str : rope_) {
    size_ += str.size_;
  }
}

ConstString::ConstString(Rope&& rope, size_t hint_size)
    : rope_(std::move(rope)), size_(hint_size) {
}

ConstString::ConstString(const Rope& rope) {
  for (const auto& str : rope) {
    size_ += str.size_;
  }
  rope_ = rope;
}

ConstString::ConstString(const Rope& rope, size_t hint_size)
    : rope_(rope), size_(hint_size) {
}

ConstString::ConstString(const String& str)
    : size_(str.size()), null_end_(true) {
  str_.reset(new char[size_ + 1], CharArrayDeleter);
  memcpy(const_cast<char*>(str_.get()), str.data(), size_ + 1);
  DCHECK(str.data()[size_] == '\0');
}

String ConstString::string_copy() const {
  CollapseRope();
  return String(str_.get(), size_);
}

ConstString ConstString::substr(size_t index, size_t length) const {
  DCHECK(index < size_);
  DCHECK(length == 0 || length + index <= size_);

  if (null_end_ && (length == 0 || length + index == size_)) {
    return ConstString(str_.get() + index, length, true);
  } else {
    return ConstString(str_.get() + index, length, false);
  }
}

const char* ConstString::data() const {
  CollapseRope();
  return str_.get();
}

const char* ConstString::c_str() const {
  CollapseRope();
  NullTerminate();
  return str_.get();
}

const char& ConstString::operator[](size_t index) const {
  DCHECK(index < size_);

  if (!rope_.empty()) {
    for (const auto& str : rope_) {
      if (index < str.size()) {
        return str[index];
      }

      index -= str.size();
    }
  }

  return *(str_.get() + index);
}

ConstString ConstString::operator+(const ConstString& other) const {
  if (!rope_.empty()) {
    Rope rope = rope_;
    rope.push_back(other);
    return rope;
  }

  return Rope{*this, other};
}

ConstString::ConstString(const char* str, size_t size, bool null_end)
    : str_(str, NoopDeleter), size_(size), null_end_(null_end) {
}

void ConstString::CollapseRope() const {
  if (rope_.empty()) {
    return;
  }

  if (rope_.size() == 1) {
    medium_ = rope_.front().medium_;
    str_ = rope_.front().str_;
    null_end_ = rope_.front().null_end_;
  } else {
    str_.reset(new char[size_ + 1], CharArrayDeleter);
    null_end_ = true;

    char* WEAK_PTR ptr = const_cast<char*>(str_.get());
    ptr[size_] = '\0';
    for (const auto& str : rope_) {
      memcpy(ptr, str.str_.get(), str.size_);
      ptr += str.size_;
    }
  }

  rope_.clear();
}

void ConstString::NullTerminate() const {
  if (null_end_) {
    return;
  }

  char* new_str = new char[size_ + 1];
  new_str[size_] = '\0';
  memcpy(new_str, str_.get(), size_);
  str_.reset(new_str, CharArrayDeleter);
  null_end_ = true;
}

}  // namespace base
}  // namespace dist_clang

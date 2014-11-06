#include <base/const_string.h>

#include <base/assert.h>
#include <base/attributes.h>

namespace dist_clang {
namespace base {

ConstString::ConstString(const char* str) : ConstString(str, true) {
  // FIXME: DCHECK() that string has only ascii chars before null-symbol.
}

ConstString::ConstString(const char* str, bool take_ownership)
    : str_(str), size_(strlen(str)), has_ownership_(take_ownership) {
  // FIXME: DCHECK() that string has only ascii chars before null-symbol.
}

ConstString::ConstString(const char str[], size_t size)
    : str_(str, std::default_delete<const char[]>()), size_(size) {
}

ConstString::ConstString(UniquePtr<char[]>& str, size_t size)
    : str_(str.release(), std::default_delete<const char[]>()), size_(size) {
}

ConstString::ConstString(const String& str) : size_(str.size()) {
  str_.reset(new char[size_], std::default_delete<const char[]>());
  memcpy(const_cast<char*>(str_.get()), str.data(), size_);
}

ConstString::ConstString(String&& str)
    : str_(str.data()),
      size_(str.size()),
      medium_(new String(std::move(str))),
      has_ownership_(false) {
  // FIXME: DCHECK() that string has only ascii chars before null-symbol.
}

ConstString::ConstString(Rope& rope) {
  if (rope.size() == 1) {
    this->operator=(rope.front());
  } else {
    for (const auto& str : rope) {
      size_ += str.size_;
    }
    str_.reset(new char[size_], std::default_delete<const char[]>());
    // FIXME: put null-terminating symbol at the end.

    char* WEAK_PTR ptr = const_cast<char*>(str_.get());
    for (const auto& str : rope) {
      memcpy(ptr, str.str_.get(), str.size_);
      ptr += str.size_;
    }
  }
}

ConstString::ConstString(Rope& rope, size_t hint_size) : size_(hint_size) {
  if (rope.size() == 1) {
    this->operator=(rope.front());
  } else {
    str_.reset(new char[hint_size], std::default_delete<const char[]>());
    // FIXME: put null-terminating symbol at the end.

    char* WEAK_PTR ptr = const_cast<char*>(str_.get());
    for (const auto& str : rope) {
      memcpy(ptr, str.str_.get(), str.size_);
      ptr += str.size_;
    }
  }
}

ConstString::~ConstString() {
  if (!has_ownership_) {
    str_.reset();
  }
}

const char& ConstString::operator[](size_t index) const {
  DCHECK(index < size_);
  return *(str_.get() + index);
}

ConstString ConstString::substr(size_t index, size_t length) const {
  DCHECK(index < size_ && (length != 0 ? length - index <= size_ : true));
  return ConstString(str_.get() + index, length, false);
}

ConstString::ConstString(const char* str, size_t size, bool take_ownership)
    : str_(str), size_(size), has_ownership_(take_ownership) {
}

}  // namespace base
}  // namespace dist_clang

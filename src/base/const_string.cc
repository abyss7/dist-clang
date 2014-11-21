#include <base/const_string.h>

#include <base/assert.h>
#include <base/attributes.h>

namespace dist_clang {

namespace {
auto NoopDeleter = [](const char*) {};
auto CharArrayDeleter = std::default_delete<const char[]>();
}

namespace base {

// static
const Literal Literal::empty = "";

ConstString::ConstString(Literal str)
    : str_(str.str_, NoopDeleter), size_(strlen(str.str_)), null_end_(true) {
  DCHECK(str.str_[size_] == '\0');
}

ConstString::ConstString(char str[])
    : str_(str, CharArrayDeleter), size_(strlen(str)), null_end_(true) {
}

ConstString::ConstString(UniquePtr<char[]>& str)
    : str_(str.release(), CharArrayDeleter),
      size_(strlen(str_.get())),
      null_end_(true) {
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

ConstString::ConstString(String* str) : ConstString(std::move(*str)) {
  DCHECK(str);
  delete str;
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

// static
ConstString ConstString::WrapString(const String& str) {
  return ConstString(str.c_str(), str.size(), true);
}

String ConstString::string_copy(bool collapse) const {
  if (collapse) {
    CollapseRope();
    return String(str_.get(), size_);
  }

  if (rope_.empty()) {
    return String(str_.get(), size_);
  }

  String result;
  result.reserve(size_);

  for (const auto& str : rope_) {
    result += str;
  }

  return result;
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

bool ConstString::operator==(const ConstString& other) const {
  if (size_ != other.size_) {
    return false;
  }
  for (size_t i = 0; i < size_; ++i) {
    if (this->operator[](i) != other[i]) {
      return false;
    }
  }

  return true;
}

size_t ConstString::find(const char* str) const {
  i64 str_size = strlen(str);
  Vector<i64> t(str_size + 1, -1);

  if (str_size == 0) {
    return 0;
  }

  for (i64 i = 1; i <= str_size; ++i) {
    auto pos = t[i - 1];
    while (pos != -1 && str[pos] != str[i - 1]) {
      pos = t[pos];
    }
    t[i] = pos + 1;
  }

  size_t sp = 0;
  i64 kp = 0;
  while (sp < size_) {
    while (kp != -1 && (kp == str_size || str[kp] != this->operator[](sp))) {
      kp = t[kp];
    }
    kp++;
    sp++;
    if (kp == str_size) {
      return sp - str_size;
    }
  }

  return String::npos;
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

ConstString ConstString::Hash(ui8 output_size) const {
  const ui64 block_size = 8;

  char* buf = new char[16];
  const ui64 double_blocks = size() / 16;
  ui64 h1 = 0, h2 = 0;
  const ui64 c1 = 0x87c37b91114253d5LLU, c2 = 0x4cf5ad432745937fLLU;

  const ui64* ptr = nullptr;
  Rope::const_iterator it;
  ui64 ptr_size = 0;

  if (rope_.empty()) {
    ptr = reinterpret_cast<const ui64*>(str_.get());
    it = rope_.end();
    ptr_size = size();
  } else {
    it = rope_.begin();
    it->CollapseRope();
    ptr = reinterpret_cast<const ui64*>(rope_.front().str_.get());
    ptr_size = it->size();
  }

  auto NextBlock = [this, &ptr, &it, &ptr_size]() -> ui64 {
    DCHECK(ptr);

    // ...[===P========...
    if (ptr_size > block_size) {
      ptr_size -= block_size;
      return *ptr++;
    }

    // ...[===P=======][===...
    if (ptr_size == block_size) {
      ui64 result = *ptr;

      if (it != rope_.end()) {
        ++it;
      }

      if (it == rope_.end()) {
        ptr_size = 0;
        ptr = nullptr;
      } else {
        it->CollapseRope();
        ptr = reinterpret_cast<const ui64*>(it->str_.get());
        ptr_size = it->size();
      }

      return result;
    }

    // ...[======P===][===...
    DCHECK(it != rope_.end());

    union {
      char str[block_size];
      ui64 num;
    } result;
    ui64 i = ptr_size;

    memcpy(result.str, ptr, ptr_size);

    ++it;
    DCHECK(it != rope_.end());
    it->CollapseRope();
    ptr = reinterpret_cast<const ui64*>(it->str_.get());
    ptr_size = it->size();

    while (i < block_size) {
      if (ptr_size <= block_size - i) {
        memcpy(result.str + i, ptr, ptr_size);
        i += ptr_size;
        ++it;
        if (it == rope_.end()) {
          ptr_size = 0;
          ptr = nullptr;
          DCHECK(i == block_size);
        } else {
          it->CollapseRope();
          ptr = reinterpret_cast<const ui64*>(it->str_.get());
          ptr_size = it->size();
        }
      } else {
        memcpy(result.str + i, ptr, block_size - i);
        i = block_size;
        ptr_size -= block_size - i;
        ptr = reinterpret_cast<const ui64*>(it->str_.get() + block_size - i);
      }
    }

    DCHECK(i == block_size);

    return result.num;
  };

  // Body.

  auto rotl64 = [](ui64 x, i8 r) -> ui64 { return (x << r) | (x >> (64 - r)); };

  auto fmix64 = [](ui64 k) -> ui64 {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdLLU;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53LLU;
    k ^= k >> 33;

    return k;
  };

  for (ui64 i = 0; i < double_blocks; ++i) {
    ui64 k1 = NextBlock(), k2 = NextBlock();

    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;

    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;

    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;

    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  // Tail.

  const ui64 tail = double_blocks * 16;

  ui64 k1 = 0, k2 = 0;

  switch (size() & 15) {
    case 15:
      k2 ^= ((ui64)this->operator[](tail + 14)) << 48;
    case 14:
      k2 ^= ((ui64)this->operator[](tail + 13)) << 40;
    case 13:
      k2 ^= ((ui64)this->operator[](tail + 12)) << 32;
    case 12:
      k2 ^= ((ui64)this->operator[](tail + 11)) << 24;
    case 11:
      k2 ^= ((ui64)this->operator[](tail + 10)) << 16;
    case 10:
      k2 ^= ((ui64)this->operator[](tail + 9)) << 8;
    case 9:
      k2 ^= ((ui64)this->operator[](tail + 8)) << 0;
      k2 *= c2;
      k2 = rotl64(k2, 33);
      k2 *= c1;
      h2 ^= k2;

    case 8:
      k1 ^= ((ui64)this->operator[](tail + 7)) << 56;
    case 7:
      k1 ^= ((ui64)this->operator[](tail + 6)) << 48;
    case 6:
      k1 ^= ((ui64)this->operator[](tail + 5)) << 40;
    case 5:
      k1 ^= ((ui64)this->operator[](tail + 4)) << 32;
    case 4:
      k1 ^= ((ui64)this->operator[](tail + 3)) << 24;
    case 3:
      k1 ^= ((ui64)this->operator[](tail + 2)) << 16;
    case 2:
      k1 ^= ((ui64)this->operator[](tail + 1)) << 8;
    case 1:
      k1 ^= ((ui64)this->operator[](tail + 0)) << 0;
      k1 *= c1;
      k1 = rotl64(k1, 31);
      k1 *= c2;
      h1 ^= k1;
  };

  // Finalization.

  h1 ^= size();
  h2 ^= size();

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  ((ui64*)buf)[0] = h1;
  ((ui64*)buf)[1] = h2;

  return Immutable(buf, std::min<ui8>(16u, output_size));
}

ConstString::ConstString(const char* WEAK_PTR str, size_t size, bool null_end)
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

  UniquePtr<char[]> new_str(new char[size_ + 1]);
  new_str[size_] = '\0';
  memcpy(new_str.get(), str_.get(), size_);
  str_.reset(new_str.release(), CharArrayDeleter);
  null_end_ = true;
}

}  // namespace base
}  // namespace dist_clang

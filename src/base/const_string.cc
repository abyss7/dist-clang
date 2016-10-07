#include <base/const_string.h>

#include <base/assert.h>
#include <base/attributes.h>

#include STL(algorithm)

#if !defined(OS_WIN)
#include <sys/mman.h>
#endif

namespace dist_clang {

namespace {
auto NoopDeleter = [](const char*) {};
auto CharArrayDeleter = std::default_delete<const char[]>();
}  // namespace

namespace base {

// static
const Literal Literal::empty = "";

ConstString::ConstString() : assignable_(true), assign_once_(true) {}

ConstString::ConstString(bool assignable) : assignable_(assignable) {
  DCHECK(assignable);
}

ConstString::ConstString(Literal str)
    : internals_(
          new Internal{.string = {str.str_, NoopDeleter}, .null_end = true}),
      size_(strlen(str.str_)) {
  DCHECK(str.str_[size_] == '\0');
}

ConstString::ConstString(char str[])
    : internals_(
          new Internal{.string = {str, CharArrayDeleter}, .null_end = true}),
      size_(strlen(str)) {}

ConstString::ConstString(UniquePtr<char[]>& str)
    : internals_(new Internal{.string = {str.release(), CharArrayDeleter},
                              .null_end = true}),
      size_(strlen(internals_->string.get())) {}

ConstString::ConstString(char str[], size_t size)
    : internals_(new Internal{.string = {str, CharArrayDeleter}}),
      size_(size) {}

#if !defined(OS_WIN)
ConstString::ConstString(void* str, size_t size)
    : internals_(new Internal{
          .string = {reinterpret_cast<char*>(str),
                     [str, size](const char*) { munmap(str, size); }}}),
      size_(size) {}
#endif

ConstString::ConstString(UniquePtr<char[]>& str, size_t size)
    : internals_(new Internal{.string = {str.release(), CharArrayDeleter}}),
      size_(size) {}

ConstString::ConstString(String&& str) {
  SharedPtr<String> medium(new String(std::move(str)));
  SharedPtr<const char> string(medium->data(), NoopDeleter);
  size_ = medium->size();

  DCHECK(medium->data()[size_] == '\0');

  internals_.reset(
      new Internal{.medium = medium, .string = string, .null_end = true});
}

ConstString::ConstString(String* str) : ConstString(std::move(*str)) {
  DCHECK(str);
  delete str;
}

ConstString::ConstString(Rope&& rope)
    : internals_(new Internal{.rope = std::move(rope)}) {
  for (const auto& str : internals_->rope) {
    size_ += str.size_;
  }
}

ConstString::ConstString(Rope&& rope, size_t hint_size)
    : internals_(new Internal{.rope = std::move(rope)}), size_(hint_size) {}

ConstString::ConstString(const Rope& rope)
    : internals_(new Internal{.rope = rope}) {
  for (const auto& str : rope) {
    size_ += str.size_;
  }
}

ConstString::ConstString(const Rope& rope, size_t hint_size)
    : internals_(new Internal{.rope = rope}), size_(hint_size) {}

ConstString::ConstString(ConstString& str, size_t size) {
  size_ = std::min(size, str.size());
  internals_.reset(new Internal{
      .string = {new char[size_ + 1], CharArrayDeleter}, .null_end = true});
  memcpy(const_cast<char*>(internals_->string.get()), str.data(), size_);
  const_cast<char*>(internals_->string.get())[size_] = '\0';
}

ConstString::ConstString(const String& str)
    : internals_(
          new Internal{.string = {new char[str.size() + 1], CharArrayDeleter},
                       .null_end = true}),
      size_(str.size()) {
  memcpy(const_cast<char*>(internals_->string.get()), str.data(), size_ + 1);
  DCHECK(str.data()[size_] == '\0');
}

// static
ConstString ConstString::WrapString(const String& str) {
  return ConstString(str.c_str(), str.size(), true);
}

String ConstString::string_copy(bool collapse) {
  auto internals = internals_;

  if (collapse) {
    internals = CollapseRope();
    DCHECK(internals->string || !size_);
    return String(internals->string.get(), size_);
  }

  if (internals->rope.empty()) {
    DCHECK(internals->string || !size_);
    return String(internals->string.get(), size_);
  }

  String result;
  result.reserve(size_);

  DCHECK(!size_ || !internals->rope.empty());
  for (const auto& str : internals->rope) {
    result += str;
  }

  return result;
}

String ConstString::string_copy() const {
  auto internals = internals_;

  if (internals->rope.empty()) {
    DCHECK(internals->string || !size_);
    return String(internals->string.get(), size_);
  }

  String result;
  result.reserve(size_);

  DCHECK(!size_ || !internals->rope.empty());
  for (const auto& str : internals->rope) {
    result += str;
  }

  return result;
}

void ConstString::assign(const ConstString& other) {
  DCHECK(assignable_);

  internals_ = other.internals_;
  size_ = other.size_;

  if (assign_once_) {
    assignable_ = false;
  }
}

const char* ConstString::data() {
  auto internals = CollapseRope();
  DCHECK(internals->string || !size_);
  return internals->string.get();
}

const char* ConstString::c_str() {
  CollapseRope();
  auto internals = NullTerminate();
  DCHECK(internals->string);
  return internals->string.get();
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

  auto internals = internals_;

  if (!internals->rope.empty()) {
    for (const auto& str : internals->rope) {
      if (index < str.size()) {
        return str[index];
      }

      index -= str.size();
    }
  }

  DCHECK(internals->string);
  return *(internals->string.get() + index);
}

ConstString ConstString::operator+(const ConstString& other) const {
  auto internals = internals_;

  if (!internals->rope.empty()) {
    Rope rope = internals->rope;
    rope.push_back(other);
    return rope;
  }

  return Rope{*this, other};
}

ConstString ConstString::Hash(ui8 output_size) const {
  // Implements the algorithm MurmurHash3 for x64 with 128 bits.

  if (empty()) {
    // FIXME: replace with a real value for an empty string.
    return {new char[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 16};
  }

  const ui64 block_size = 8;

  char* buf = new char[16];
  const ui64 double_blocks = size() / 16;
  ui64 h1 = 0, h2 = 0;
  const ui64 c1 = 0x87c37b91114253d5LLU, c2 = 0x4cf5ad432745937fLLU;

  const ui64* ptr = nullptr;
  Rope::iterator it;
  ui64 ptr_size = 0;

  auto internals = internals_;
  auto rope = internals->rope;

  if (internals->rope.empty()) {
    DCHECK(internals->string);
    ptr = reinterpret_cast<const ui64*>(internals->string.get());
    it = rope.end();
    ptr_size = size();
  } else {
    it = rope.begin();
    auto rope_internals = it->CollapseRope();
    ptr = reinterpret_cast<const ui64*>(rope_internals->string.get());
    ptr_size = it->size();
  }

  auto NextBlock = [&rope, &ptr, &it, &ptr_size]() -> ui64 {
    DCHECK(ptr);

    // ...[===P=====B==...
    if (ptr_size > block_size) {
      ptr_size -= block_size;
      return *ptr++;
    }

    // ...[===P======B][===...
    if (ptr_size == block_size) {
      ui64 result = *ptr;

      if (it != rope.end()) {
        ++it;
      }

      if (it == rope.end()) {
        ptr_size = 0;
        ptr = nullptr;
      } else {
        auto rope_internals = it->CollapseRope();
        ptr = reinterpret_cast<const ui64*>(rope_internals->string.get());
        ptr_size = it->size();
      }

      return result;
    }

    // ...[======P===][=B=...
    DCHECK(it != rope.end());  // FIXME: what if the |rope| is empty?

    union {
      char str[block_size];
      ui64 num;
    } result;
    ui64 i = ptr_size;

    memcpy(result.str, ptr, ptr_size);

    ++it;
    DCHECK(it != rope.end());
    auto rope_internals = it->CollapseRope();
    ptr = reinterpret_cast<const ui64*>(rope_internals->string.get());
    ptr_size = it->size();

    while (i < block_size) {
      if (ptr_size <= block_size - i) {
        memcpy(result.str + i, ptr, ptr_size);
        i += ptr_size;
        ++it;
        if (it == rope.end()) {
          ptr_size = 0;
          ptr = nullptr;
          DCHECK(i == block_size);
        } else {
          auto rope_internals = it->CollapseRope();
          ptr = reinterpret_cast<const ui64*>(rope_internals->string.get());
          ptr_size = it->size();
        }
      } else {
        memcpy(result.str + i, ptr, block_size - i);
        ptr_size -= block_size - i;
        auto rope_internals = it->internals_;
        DCHECK(rope_internals->string);
        ptr = reinterpret_cast<const ui64*>(rope_internals->string.get() +
                                            block_size - i);
        i = block_size;
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
      k2 ^= ((ui64) this->operator[](tail + 14)) << 48;
    case 14:
      k2 ^= ((ui64) this->operator[](tail + 13)) << 40;
    case 13:
      k2 ^= ((ui64) this->operator[](tail + 12)) << 32;
    case 12:
      k2 ^= ((ui64) this->operator[](tail + 11)) << 24;
    case 11:
      k2 ^= ((ui64) this->operator[](tail + 10)) << 16;
    case 10:
      k2 ^= ((ui64) this->operator[](tail + 9)) << 8;
    case 9:
      k2 ^= ((ui64) this->operator[](tail + 8)) << 0;
      k2 *= c2;
      k2 = rotl64(k2, 33);
      k2 *= c1;
      h2 ^= k2;

    case 8:
      k1 ^= ((ui64) this->operator[](tail + 7)) << 56;
    case 7:
      k1 ^= ((ui64) this->operator[](tail + 6)) << 48;
    case 6:
      k1 ^= ((ui64) this->operator[](tail + 5)) << 40;
    case 5:
      k1 ^= ((ui64) this->operator[](tail + 4)) << 32;
    case 4:
      k1 ^= ((ui64) this->operator[](tail + 3)) << 24;
    case 3:
      k1 ^= ((ui64) this->operator[](tail + 2)) << 16;
    case 2:
      k1 ^= ((ui64) this->operator[](tail + 1)) << 8;
    case 1:
      k1 ^= ((ui64) this->operator[](tail + 0)) << 0;
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
    : internals_(
          new Internal{.string = {str, NoopDeleter}, .null_end = null_end}),
      size_(size) {}

ConstString::InternalPtr ConstString::CollapseRope() {
  auto internals = internals_;

  if (internals->rope.empty()) {
    return internals;
  }

  InternalPtr new_internals;
  if (internals->rope.size() == 1) {
    new_internals = internals->rope.front().internals_;
  } else {
    SharedPtr<const char> new_string(new char[size_ + 1], CharArrayDeleter);
    char* WEAK_PTR ptr = const_cast<char*>(new_string.get());
    ptr[size_] = '\0';
    for (const auto& string : internals->rope) {
      memcpy(ptr, string.internals_->string.get(), string.size_);
      ptr += string.size_;
    }

    new_internals.reset(new Internal{.string = new_string, .null_end = true});
  }

  internals_ = new_internals;
  return new_internals;
}

ConstString::InternalPtr ConstString::NullTerminate() {
  auto internals = internals_;

  DCHECK([internals] { return internals->rope.empty(); }());

  if (internals->null_end) {
    return internals;
  }

  UniquePtr<char[]> new_string(new char[size_ + 1]);
  new_string[size_] = '\0';
  memcpy(new_string.get(), internals->string.get(), size_);

  InternalPtr new_internals(new Internal{
      .string = {new_string.release(), CharArrayDeleter}, .null_end = true});
  return internals_ = new_internals;
}

}  // namespace base
}  // namespace dist_clang

#include <base/file/file.h>

#include <base/c_utils.h>
#include <base/const_string.h>
#include <base/string_utils.h>

#include STL(cstdio)
#include STL_EXPERIMENTAL(filesystem)
#include STL(system_error)

namespace dist_clang {

namespace {

bool GetStatus(const Path& path,
               std::experimental::filesystem::file_status* status,
               String* error) {
  CHECK(status);
  std::error_code ec;
  *status = std::experimental::filesystem::status(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

}  // anonymous namespace

namespace base {

// static
bool File::IsFile(const Path& path, String* error) {
  std::experimental::filesystem::file_status status;
  if (!GetStatus(path, &status, error)) {
    return false;
  }
  return !std::experimental::filesystem::is_directory(status);
}

bool File::Hash(Immutable* output, const List<Literal>& skip_list,
                String* error) {
  DCHECK(IsValid());

  Immutable tmp_output;
  if (!Read(&tmp_output, error)) {
    return false;
  }

  for (const char* skip : skip_list) {
    if (tmp_output.find(skip) != String::npos) {
      if (error) {
        error->assign("Skip-list hit: " + String(skip));
      }
      return false;
    }
  }

  output->assign(base::Hexify(tmp_output.Hash()));
  return true;
}

// static
ui64 File::Size(const Path& path, String* error) {
  std::error_code ec;
  const auto file_size = std::experimental::filesystem::file_size(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return 0;
  }
  return static_cast<ui64>(file_size);
}

// static
bool File::Read(const Path& path, Immutable* output, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Read(output, error);
}

// static
bool File::Exists(const Path& path, String* error) {
  std::experimental::filesystem::file_status status;
  if (!GetStatus(path, &status, error)) {
    return false;
  }
  return std::experimental::filesystem::exists(status) &&
         !std::experimental::filesystem::is_directory(status);
}

// static
bool File::Hash(const Path& path, Immutable* output,
                const List<Literal>& skip_list, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Hash(output, skip_list, error);
}

// static
bool File::Copy(const Path& src_path, const Path& dst_path, String* error) {
  File src(src_path);

  if (!src.IsValid()) {
    src.GetCreationError(error);
    return false;
  }

  return src.CopyInto(dst_path, error);
}

// static
bool File::Move(const Path& src, const Path& dst, String* error) {
  std::error_code ec;
  std::experimental::filesystem::rename(src, dst, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

// static
bool File::Delete(const Path& path, String* error) {
  if (std::remove(path.c_str())) {
    GetLastError(error);
    return false;
  }

  return true;
}

}  // namespace base

}  // namespace dist_clang

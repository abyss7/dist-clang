#include <base/file/file.h>

#include <base/const_string.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include STL(thread)

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

TEST(FileTest, NotInheritedByChildren) {
  const TemporaryDir temp_dir;
  const Path path = temp_dir.path() / "file";
  ASSERT_TRUE(File::Write(path, "content"_l));

  // The other threads start the compilers at the same time.
  const File file(path);
  ASSERT_TRUE(file.IsValid());
  EXPECT_TRUE(fcntl(file.native(), F_GETFD) & FD_CLOEXEC);
}

TEST(FileTest, ReadIsSnapshot) {
  const TemporaryDir temp_dir;
  const Path path = temp_dir.path() / "file";
  ASSERT_TRUE(File::Write(path, "old contents"_l));

  Immutable contents;
  ASSERT_TRUE(File::Read(path, &contents));

  // E.g. a generator rewrites the header in place, while it's hashed.
  const int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
  ASSERT_NE(-1, fd);
  ASSERT_EQ(12, write(fd, "new contents", 12));
  close(fd);
  EXPECT_EQ("old contents"_l, contents);

  // The truncated file mustn't take down the reader either.
  ASSERT_EQ(0, truncate(path.c_str(), 0));
  EXPECT_EQ("old contents"_l, contents);
}

TEST(FileTest, Read) {
  const auto expected_content = "All your base are belong to us"_l;
  const TemporaryDir temp_dir;
  const auto file_path = temp_dir.path() / "file";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, expected_content, expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);

  Immutable content(true);
  // |content| is assignable since we use it many times during test.

  String error;
  EXPECT_TRUE(File::Read(file_path, &content, &error)) << error;
  EXPECT_EQ(expected_content, content);

  File file(file_path);
  EXPECT_TRUE(file.Read(&content, &error)) << error;
  EXPECT_EQ(expected_content, content);

  // Can't open directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Read(&content));
}

TEST(FileTest, Write) {
  const auto expected_content = "All your base are belong to us"_l;
  const TemporaryDir temp_dir;
  const auto file_path = temp_dir.path() / "file";

  String error;
  EXPECT_TRUE(File::Write(file_path, expected_content, &error)) << error;

  Vector<char> content(expected_content.size());
  int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_NE(-1, fd);
  int size = read(fd, content.data(), expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);

  EXPECT_EQ(expected_content, String(content.data(), expected_content.size()));

  // Can't write to directory.
  EXPECT_FALSE(File::Write(temp_dir, expected_content));
}

TEST(FileTest, FailedWriteKeepsOldFile) {
  // E.g. out of the space on the disk: the truncated file isn't published.
  const TemporaryDir temp_dir;
  const auto path = temp_dir.path() / "file";
  ASSERT_TRUE(File::Write(path, "old contents"_l));

  struct rlimit old_limit;
  ASSERT_EQ(0, getrlimit(RLIMIT_FSIZE, &old_limit));
  auto* old_handler = signal(SIGXFSZ, SIG_IGN);
  struct rlimit limit = old_limit;
  limit.rlim_cur = 4096;
  ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &limit));
  String error;
  const bool written =
      File::Write(path, Immutable(String(10000, 'n')), &error);
  ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &old_limit));
  signal(SIGXFSZ, old_handler);

  EXPECT_FALSE(written);
  EXPECT_FALSE(error.empty());
  Immutable contents;
  ASSERT_TRUE(File::Read(path, &contents));
  EXPECT_EQ("old contents"_l, contents);
  // No temporary files are left.
  ui32 files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(temp_dir.path())) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(1u, files);
}

TEST(FileTest, ParallelWrites) {
  // The writers of the same file don't spoil each other's contents.
  const TemporaryDir temp_dir;
  const auto path = temp_dir.path() / "file";
  const String first(100000, 'a'), second(50000, 'b');

  Atomic<bool> stop = false, spoiled = false;
  Vector<std::thread> writers;
  for (const String* contents : {&first, &second}) {
    writers.emplace_back([&, contents] {
      while (!stop) {
        File::Write(path, Immutable(String(*contents)));
      }
    });
  }
  for (int i = 0; i < 2000 && !spoiled; ++i) {
    Immutable read;
    if (File::Read(path, &read) && read.string_copy() != first &&
        read.string_copy() != second) {
      spoiled = true;
    }
  }
  stop = true;
  for (auto& writer : writers) {
    writer.join();
  }
  EXPECT_FALSE(spoiled);
}

TEST(FileTest, Size) {
  const TemporaryDir temp_dir;
  const auto file_path = temp_dir.path() / "file";
  const String content = "1234567890";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, content.data(), content.size());
  ASSERT_EQ(content.size(), static_cast<size_t>(size));
  close(fd);

  EXPECT_EQ(content.size(), File::Size(file_path));

  File file(file_path);
  EXPECT_EQ(content.size(), file.Size());

  // Can't get size of directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Size());
}

TEST(FileTest, Hash) {
  const auto content = "All your base are belong to us"_l;
  const auto expected_hash = "c9e92e37df1e856cbd0abffe104225b8"_l;
  const TemporaryDir temp_dir;
  const auto file_path = temp_dir.path() / "file";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, content, content.size());
  ASSERT_EQ(content.size(), static_cast<size_t>(size));
  close(fd);

  Immutable hash(true);
  // |hash| is assignable since we use it many times during test.

  String error;
  EXPECT_TRUE(File::Hash(file_path, &hash, List<Literal>(), &error)) << error;
  EXPECT_EQ(expected_hash, hash);
  EXPECT_FALSE(File::Hash(file_path, &hash, {"belong"_l}, &error));
  EXPECT_TRUE(File::Hash(file_path, &hash, {"bycicle"_l}, &error)) << error;
  EXPECT_EQ(expected_hash, hash);

  File file(file_path);
  EXPECT_TRUE(file.Hash(&hash, List<Literal>(), &error)) << error;
  EXPECT_EQ(expected_hash, hash);
  EXPECT_FALSE(file.Hash(&hash, {"belong"_l}, &error));
  EXPECT_TRUE(file.Hash(&hash, {"bycicle"_l}, &error)) << error;

  // Can't hash a directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Hash(&hash));
}

TEST(FileTest, Copy) {
  const auto expected_content1 = "All your base are belong to us"_l;
  const auto expected_content2 = "Nothing lasts forever"_l;
  const TemporaryDir temp_dir;
  const auto file1 = temp_dir.path() / "1";
  const auto file2 = temp_dir.path() / "2";
  const auto file3 = temp_dir.path() / "3";

  ASSERT_TRUE(File::Write(file1, expected_content1));
  ASSERT_TRUE(File::Copy(file1, file2));

  struct stat st;
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  Immutable content(true);
  // |content| is assignable since we use it many times during test.

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  String error;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  ASSERT_TRUE(File::Read(file2, &content, &error)) << error;
  EXPECT_EQ(expected_content1, content);

  ASSERT_TRUE(File::Write(file3, expected_content2));
  ASSERT_TRUE(File::Copy(file3, file2));

  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  ASSERT_TRUE(File::Read(file2, &content));
  EXPECT_EQ(expected_content2, content);
}

TEST(FileTest, Clone) {
  const auto content1 = "All your base are belong to us"_l;
  const auto content2 = "Something else"_l;
  const TemporaryDir temp_dir;
  const auto src = temp_dir.path() / "src";
  const auto dst = temp_dir.path() / "dst";

  ASSERT_TRUE(File::Write(src, content1));
  ASSERT_TRUE(File::Write(dst, content2));

  File::CloneMode mode;
  String error;
  ASSERT_TRUE(File::Clone(src, dst, true, &mode, &error)) << error;
  EXPECT_NE(File::CloneMode::COPY, mode);

  Immutable content;
  ASSERT_TRUE(File::Read(dst, &content));
  EXPECT_EQ(content1, content);

  struct stat src_st, dst_st;
  ASSERT_EQ(0, stat(src.c_str(), &src_st));
  ASSERT_EQ(0, stat(dst.c_str(), &dst_st));
  EXPECT_EQ(mode == File::CloneMode::HARDLINK, src_st.st_ino == dst_st.st_ino);

  // Without hard links the files never share the inode.
  ASSERT_TRUE(File::Clone(src, dst, false, &mode, &error)) << error;
  EXPECT_NE(File::CloneMode::HARDLINK, mode);
  ASSERT_EQ(0, stat(dst.c_str(), &dst_st));
  EXPECT_NE(src_st.st_ino, dst_st.st_ino);
  Immutable content2_read;
  ASSERT_TRUE(File::Read(dst, &content2_read));
  EXPECT_EQ(content1, content2_read);

  // No temporary files are left.
  ui32 files = 0;
  for ([[maybe_unused]] const auto& entry :
       std::filesystem::directory_iterator(temp_dir.path())) {
    ++files;
  }
  EXPECT_EQ(2u, files);

  EXPECT_FALSE(File::Clone(temp_dir.path() / "missing", dst, true));
}

TEST(FileTest, CloneOverTheSameFile) {
  const TemporaryDir temp_dir;
  const auto src = temp_dir.path() / "src";
  const auto dst = temp_dir.path() / "dst";

  ASSERT_TRUE(File::Write(src, "content"_l));
  File::CloneMode mode;
  ASSERT_TRUE(File::Clone(src, dst, true, &mode));
  if (mode != File::CloneMode::HARDLINK) {
    return;
  }

  // The rename of a hard link over another one of the same file does nothing
  // - the temporary link mustn't stay.
  ASSERT_TRUE(File::Clone(src, dst, true, &mode));
  struct stat st;
  ASSERT_EQ(0, stat(src.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  ui32 files = 0;
  for ([[maybe_unused]] const auto& entry :
       std::filesystem::directory_iterator(temp_dir.path())) {
    ++files;
  }
  EXPECT_EQ(2u, files);
}

TEST(FileTest, Link) {
  const auto expected_content = "All your base are belong to us"_l;
  const TemporaryDir temp_dir;
  const auto file1 = temp_dir.path() / "1";
  const auto file2 = temp_dir.path() / "2";
  const auto file3 = temp_dir.path() / "3";

  ASSERT_TRUE(File::Write(file1, expected_content));
  ASSERT_TRUE(File::Link(file1, file2));

  struct stat st;
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  auto inode = st.st_ino;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_EQ(inode, st.st_ino);

  ASSERT_TRUE(File::Write(file3, expected_content));
  ASSERT_TRUE(File::Link(file3, file2));

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  inode = st.st_ino;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_NE(inode, st.st_ino);

  inode = st.st_ino;
  ASSERT_EQ(0, stat(file3.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_EQ(inode, st.st_ino);
}

}  // namespace base
}  // namespace dist_clang

#include <daemon/outputs.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <sys/stat.h>

namespace dist_clang {
namespace daemon {

namespace {

ui64 Inode(const Path& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
}

}  // namespace

TEST(OutputsTest, HasSameContents) {
  const base::TemporaryDir temp_dir;
  const auto output = temp_dir.path() / "test.o";
  const auto cache_file = temp_dir.path() / "cache.o";
  ASSERT_TRUE(base::File::Write(cache_file, "object"_l));

  Immutable hash;
  ASSERT_TRUE(base::File::Hash(cache_file, &hash));

  EXPECT_FALSE(HasSameContents(output, cache_file, hash.string_copy()));
  EXPECT_FALSE(HasSameContents(output, "object"_l));

  ASSERT_TRUE(base::File::Write(output, "object"_l));
  EXPECT_TRUE(HasSameContents(output, cache_file, hash.string_copy()));
  EXPECT_TRUE(HasSameContents(output, "object"_l));
  EXPECT_FALSE(HasSameContents(output, cache_file, String()));

  ASSERT_TRUE(base::File::Write(output, "OBJECT"_l));
  EXPECT_FALSE(HasSameContents(output, cache_file, hash.string_copy()));
  EXPECT_FALSE(HasSameContents(output, "object"_l));

  // The same file is the same without hashing.
  ASSERT_TRUE(base::File::Clone(cache_file, output, true));
  if (Inode(output) == Inode(cache_file)) {
    EXPECT_TRUE(HasSameContents(output, cache_file, String()));
  }
}

TEST(OutputsTest, RestoreSameOutput) {
  const base::TemporaryDir temp_dir;
  const auto output = temp_dir.path() / "test.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  const auto inode = Inode(output);

  {
    PreviousOutput previous(output);
    previous.Save();
    EXPECT_FALSE(base::File::Exists(output));

    ASSERT_TRUE(base::File::Write(output, "object"_l));
    EXPECT_TRUE(previous.RestoreIfSame());
  }

  EXPECT_EQ(inode, Inode(output));
  EXPECT_FALSE(base::File::Exists(output.string() + ".dc-prev"));
}

TEST(OutputsTest, KeepChangedOutput) {
  const base::TemporaryDir temp_dir;
  const auto output = temp_dir.path() / "test.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));

  {
    PreviousOutput previous(output);
    previous.Save();
    ASSERT_TRUE(base::File::Write(output, "new object"_l));
    EXPECT_FALSE(previous.RestoreIfSame());
  }

  Immutable contents;
  ASSERT_TRUE(base::File::Read(output, &contents));
  EXPECT_EQ("new object"_l, contents);
  EXPECT_FALSE(base::File::Exists(output.string() + ".dc-prev"));
}

TEST(OutputsTest, NoNewOutput) {
  const base::TemporaryDir temp_dir;
  const auto output = temp_dir.path() / "test.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));

  {
    // E.g. the compilation failed.
    PreviousOutput previous(output);
    previous.Save();
    EXPECT_FALSE(previous.RestoreIfSame());
  }

  EXPECT_FALSE(base::File::Exists(output));
  EXPECT_FALSE(base::File::Exists(output.string() + ".dc-prev"));

  // Nothing to save.
  PreviousOutput previous(output);
  previous.Save();
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  EXPECT_FALSE(previous.RestoreIfSame());
}

TEST(OutputsTest, RemoveStalePreviousOutput) {
  // The daemon exits during the compilation, and leaves the previous output
  // aside: it pins the blob of the cache, that it's linked with.
  const base::TemporaryDir temp_dir;
  const auto output = temp_dir.path() / "test.o";
  const auto stale = Path(output.string() + ".dc-prev");

  // The output is removed since.
  ASSERT_TRUE(base::File::Write(stale, "object"_l));
  {
    PreviousOutput previous(output);
    previous.Save();
    EXPECT_FALSE(base::File::Exists(stale));
    EXPECT_FALSE(previous.RestoreIfSame());
  }

  // The unchanged outputs aren't kept.
  ASSERT_TRUE(base::File::Write(stale, "object"_l));
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  {
    PreviousOutput previous(output);
    previous.Delete();
    EXPECT_FALSE(base::File::Exists(output));
    EXPECT_FALSE(base::File::Exists(stale));
  }
}

}  // namespace daemon
}  // namespace dist_clang

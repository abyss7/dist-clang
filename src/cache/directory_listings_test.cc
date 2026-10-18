#include <cache/directory_listings.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <utime.h>

namespace dist_clang {
namespace cache {

TEST(DirectoryListingsTest, NamesOfDirectory) {
  const base::TemporaryDir temp_dir;
  const auto dir = temp_dir.path() / "dir";
  ASSERT_TRUE(base::CreateDirectory(dir));
  ASSERT_TRUE(base::File::Write(dir / "a.h", "a"_l));

  DirectoryListings listings;
  auto listing = listings.Get(dir.string());
  ASSERT_TRUE(listing);
  EXPECT_TRUE(listing->contains("a.h"));
  EXPECT_FALSE(listing->contains("b.h"));
  // The missing directories have no names.
  EXPECT_FALSE(listings.Get((temp_dir.path() / "missing").string()));
}

TEST(DirectoryListingsTest, NewNamesAreSeen) {
  const base::TemporaryDir temp_dir;
  const auto dir = temp_dir.path() / "dir";
  ASSERT_TRUE(base::CreateDirectory(dir));
  ASSERT_TRUE(base::File::Write(dir / "a.h", "a"_l));
  DirectoryListings listings;

  // The directory, modified long ago, is remembered - until it's modified.
  struct utimbuf old_time = {time(nullptr) - 10, time(nullptr) - 10};
  ASSERT_EQ(0, utime(dir.c_str(), &old_time));
  EXPECT_FALSE(listings.Get(dir.string())->contains("b.h"));
  ASSERT_TRUE(base::File::Write(dir / "b.h", "b"_l));
  EXPECT_TRUE(listings.Get(dir.string())->contains("b.h"));

  // The directory, modified just now, may change within the same tick of the
  // clock of the file system: it's read again every time.
  EXPECT_FALSE(listings.Get(dir.string())->contains("c.h"));
  ASSERT_TRUE(base::File::Write(dir / "c.h", "c"_l));
  EXPECT_TRUE(listings.Get(dir.string())->contains("c.h"));
}

}  // namespace cache
}  // namespace dist_clang

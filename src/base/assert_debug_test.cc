#undef NDEBUG
#include "base/assert.h"

#include "gtest/gtest.h"

namespace dist_clang {
namespace base {

TEST(AssertDebugTest, FailureStackTrace) {
  // FIXME: a gtest implementation-dependent test.
  const char* expected =
      "Assertion failed: false\n"
      "  dist_clang::base::AssertDebugTest_FailureStackTrace_Test::TestBody\\("
      "\\)\n"
      "  void testing::internal::HandleSehExceptionsInMethodIfSupported<testing"
      "::Test, void>\\(testing::Test\\*, void \\(testing::Test::\\*\\)\\(\\), c"
      "har const\\*\\)\n"
      "  void testing::internal::HandleExceptionsInMethodIfSupported<testing::T"
      "est, void>\\(testing::Test\\*, void \\(testing::Test::\\*\\)\\(\\), char"
      " const\\*\\)\n"
      "  testing::Test::Run\\(\\)\n"
      "  testing::TestInfo::Run\\(\\)\n"
      "  testing::TestCase::Run\\(\\)\n"
      "  testing::internal::UnitTestImpl::RunAllTests\\(\\)\n"
      "  bool testing::internal::HandleSehExceptionsInMethodIfSupported<testing"
      "::internal::UnitTestImpl, bool>\\(testing::internal::UnitTestImpl\\*, bo"
      "ol \\(testing::internal::UnitTestImpl::\\*\\)\\(\\), char const\\*\\)\n"
      "  bool testing::internal::HandleExceptionsInMethodIfSupported<testing::i"
      "nternal::UnitTestImpl, bool>\\(testing::internal::UnitTestImpl\\*, bool "
      "\\(testing::internal::UnitTestImpl::\\*\\)\\(\\), char const\\*\\)\n"
      "  testing::UnitTest::Run\\(\\)\n"
      "  RUN_ALL_TESTS\\(\\)\n"
      "  main\n"
      "  __libc_start_main";
  ASSERT_THROW_STD(CHECK(false), expected);
  ASSERT_THROW_STD(DCHECK(false), expected);
  ASSERT_THROW_STD(DCHECK_O_EVAL(false), expected);
  ASSERT_THROW_STD(NOTREACHED(), expected);
}

TEST(AssertDebugTest, ExpressionEvaluation) {
  int i = 0;
  auto expr = [](int& a) -> bool { ++a; return true; };
  ASSERT_NO_THROW(DCHECK_O_EVAL(expr(i)));
  ASSERT_EQ(1, i);
}

}  // namespace base
}  // namespace dist_clang

#pragma once

#include <gtest/gtest_prod.h>

// Forward declaration of the test class - for |FRIEND_TEST| in another
// namespace.
#define FORWARD_TEST(test_case_name, test_name) \
  class test_case_name##_##test_name##_Test

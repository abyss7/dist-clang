#pragma once

#include <gtest/gtest.h>

#include <regex>
#include <stdexcept>
#include <string>

// Checks that |statement| throws an exception derived from |std::exception|,
// which |what()| matches the |regex|.
#define DC_TEST_THROW_STD_(statement, pattern, assertion)                      \
  do {                                                                       \
    bool dc_caught = false;                                                  \
    std::string dc_what;                                                     \
    try {                                                                    \
      statement;                                                             \
    } catch (const std::exception& e) {                                      \
      dc_caught = true;                                                      \
      dc_what = e.what();                                                    \
    }                                                                        \
    assertion(dc_caught) << "Expected: " #statement " throws std::exception"; \
    if (dc_caught) {                                                         \
      assertion(std::regex_search(dc_what,                                   \
                                  std::regex((pattern), std::regex::extended))) \
          << "Expected what() message: " << (pattern) << std::endl             \
          << "Actual message: " << dc_what;                                  \
    }                                                                        \
  } while (false)

#define EXPECT_THROW_STD(statement, pattern) \
  DC_TEST_THROW_STD_(statement, pattern, EXPECT_TRUE)
#define ASSERT_THROW_STD(statement, pattern) \
  DC_TEST_THROW_STD_(statement, pattern, ASSERT_TRUE)

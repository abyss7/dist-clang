// NOTICE: This file should be included only inside .cc files after all other
//         includes.

#pragma once

#undef FATAL

#undef ERROR
#undef WARNING
#undef INFO
#undef VERBOSE

#undef CACHE_ERROR
#undef CACHE_WARNING
#undef CACHE_INFO
#undef CACHE_VERBOSE

#undef DB_ERROR
#undef DB_WARNING
#undef DB_INFO
#undef DB_VERBOSE

using namespace dist_clang::base::named_levels;

#define LOG(level) dist_clang::base::Log(level)
#if defined(NDEBUG)
#define DLOG(level) std::stringstream()
#else
#define DLOG(level) LOG(level)
#endif

// My luck, the gtest severity names are the same.
#define GTEST_LOG_(severity) LOG(severity) << "Google Test " #severity ": "
#define GTEST_USE_EXTERNAL_LOG_FACILITY

// NOTICE: This file should be included only inside .cc files after all other
//         includes.

#pragma once

#undef FATAL
#undef ERROR
#undef WARNING
#undef INFO
using namespace dist_clang::base::NamedLevels;

#define LOG(level) dist_clang::base::Log(level)
#if defined(NDEBUG)
#  define DLOG(level) std::stringstream()
#else
#  define DLOG(level) LOG(level)
#endif

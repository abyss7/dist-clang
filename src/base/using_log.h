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

using namespace dist_clang::base::named_levels;

#define LOG(level) dist_clang::base::Log(level)
#if defined(NDEBUG)
#define DLOG(level) std::stringstream()
#else
#define DLOG(level) LOG(level)
#endif

#pragma once

#if !defined(OS_WIN)
#define STL(file_name) <third_party/libcxx/exported/include/file_name>
#else
#if !__has_feature(cxx_exceptions)
#define _HAS_EXCEPTIONS 0
#define __uncaught_exception std::uncaught_exception
#endif  // !__has_feature(cxx_exceptions)
#define STL(file_name) <file_name>
#endif  // !defined(OS_WIN)

#define STL_EXPERIMENTAL(file_name) STL(experimental/file_name)

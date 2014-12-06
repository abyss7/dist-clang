#pragma once

#if defined(_LIBCPP_VERSION)
#define STL(file_name) <third_party/libcxx/exported/include/file_name>
#else
#define STL(file_name) <file_name>
#endif  // defined(_LIBCPP_VERSION)

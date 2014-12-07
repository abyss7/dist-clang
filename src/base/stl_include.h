#pragma once

#if !defined(OS_WIN)
#define STL(file_name) <third_party/libcxx/exported/include/file_name>
#else
#define STL(file_name) <file_name>
#endif  // !defined(OS_WIN)

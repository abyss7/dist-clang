{
  'includes': [
    '../../build/configs.gypi',
  ],

  'targets': [
    {
      'target_name': 'c++',
      'type': 'shared_library',
      'all_dependent_settings': {
        'include_dirs': [
          'include',
        ],
      },
      'defines': [
        '_LIBCPP_NO_EXCEPTIONS',
        'cxx_EXPORTS',
      ],
      'cflags': [
        '-O3',
        '-std=c++11',
        '-nostdinc++',
        '-fPIC',
        '-fno-exceptions',
        '-pedantic',
        '-Wall',
        '-W',
        '-Wno-unused-parameter',
        '-Wwrite-strings',
        '-Wno-long-long',
        '-Wno-error',
      ],
      'cflags!': [
        '-O0',  # libc++.so loses |~basic_string()| with this flag.
      ],
      'ldflags': [
        '-nodefaultlibs',
        '-lpthread',
        '-lc',
        '-lm',
      ],
      'include_dirs': [
        'include',
      ],
      'sources': [
        'src/algorithm.cpp',
        'src/bind.cpp',
        'src/chrono.cpp',
        'src/condition_variable.cpp',
        'src/debug.cpp',
        'src/exception.cpp',
        'src/future.cpp',
        'src/hash.cpp',
        'src/ios.cpp',
        'src/iostream.cpp',
        'src/locale.cpp',
        'src/memory.cpp',
        'src/mutex.cpp',
        'src/new.cpp',
        'src/optional.cpp',
        'src/random.cpp',
        'src/regex.cpp',
        'src/shared_mutex.cpp',
        'src/stdexcept.cpp',
        'src/string.cpp',
        'src/strstream.cpp',
        'src/system_error.cpp',
        'src/thread.cpp',
        'src/typeinfo.cpp',
        'src/utility.cpp',
        'src/valarray.cpp',
      ],
      'conditions': [
        ['OS=="linux"', {
          'dependencies': [
            '../libcxxabi/libcxxabi.gyp:c++abi',
          ],
          'ldflags': [
            '-lrt',
            '-lgcc_s',
          ],
        }],
        ['OS=="mac"', {
          'ldflags': [
            '-lc++abi',
          ],
        }],
      ],
      'xcode_settings': {
        'ARCHS': ['x86_64'],
      },
    },
  ],
}

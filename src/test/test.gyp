{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'unit_tests',
      'type': 'executable',
      'dependencies': [
        '../base/base.gyp:base',
        '../base/base.gyp:hash',
        '../base/base.gyp:process',
        '../client/clang.gyp:clang_helpers',
        '../daemon/clangd.gyp:file_cache',
        '../net/net.gyp:net',
        '../third_party/gmock/gmock.gyp:gmock',
        '../third_party/gtest/gtest.gyp:gtest',
        '../third_party/protobuf/protobuf.gyp:protobuf',
      ],
      'cflags!': [
        '-fno-exceptions',  # catch failures with exceptions in tests.
      ],
      'sources': [
        '../base/assert_debug_test.cc',
        '../base/assert_release_test.cc',
        '../base/file_utils_test.cc',
        '../base/hash_test.cc',
        '../base/locked_queue_test.cc',
        '../base/process_test.cc',
        '../base/string_utils_test.cc',
        '../client/clang_test.cc',
        '../daemon/file_cache_test.cc',
        'run_all_tests.cc',
      ],
      'conditions': [
        ['OS=="linux"', {
          'sources': [
            # Connection tests use epoll right now.
            '../net/connection_test.cc',
            '../net/epoll_event_loop_test.cc',
          ],
        }],
        ['OS=="mac"', {
          'sources': [
            '../net/kqueue_event_loop_test.cc',
          ],
        }],
      ],
    },
  ],
}

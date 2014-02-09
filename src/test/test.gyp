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
        '../client/clang.gyp:client',
        '../daemon/clangd.gyp:daemon',
        '../daemon/clangd.gyp:file_cache',
        '../net/net.gyp:net',
        '../third_party/gmock/gmock.gyp:gmock',
        '../third_party/gtest/gtest.gyp:gtest',
      ],
      'sources': [
        '../base/assert_debug_test.cc',
        '../base/assert_release_test.cc',
        '../base/file_utils_test.cc',
        '../base/hash_test.cc',
        '../base/locked_queue_test.cc',
        '../base/process_test.cc',
        '../base/string_utils_test.cc',
        '../base/test_process.cc',
        '../base/test_process.h',
        '../client/clang_test.cc',
        '../daemon/daemon_test.cc',
        '../daemon/file_cache_test.cc',
        '../net/test_connection.cc',
        '../net/test_connection.h',
        '../net/test_network_service.cc',
        '../net/test_network_service.h',
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

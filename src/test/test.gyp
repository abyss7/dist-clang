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
        '../base/base.gyp:process',
        '../net/net.gyp:net',
        '../third_party/gtest/gtest.gyp:gtest',
        '../third_party/protobuf/protobuf.gyp:protobuf',
      ],
      'sources': [
        '../base/process_test.cc',
        '../net/connection_test.cc',
        'run_all_tests.cc',
      ],
      'conditions': [
        ['OS=="linux"', {
          'sources': [
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

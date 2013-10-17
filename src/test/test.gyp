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
    },
  ],
}

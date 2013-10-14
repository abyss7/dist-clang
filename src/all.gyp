{
  'includes': [
    'build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'unit_tests',
      'type': 'executable',
      'dependencies': [
        'base/base.gyp:base',
        'net/net.gyp:net',
      ],
      'ldflags': [
        '-lgtest',
        '-lprotobuf',
      ],
      'sources': [
        'base/process_test.cc',
        'net/connection_test.cc',
        'test/run_all_tests.cc',
      ],
    },
    {
      'target_name': 'All',
      'type': 'none',
      'dependencies': [
        'client/clang.gyp:clang',
        'daemon/clangd.gyp:clangd',
      ],
    },
  ],
}

{
  'includes': [
    'defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'All',
      'type': 'none',
      'dependencies': [
        '../client/clang.gyp:clang',
        '../daemon/clangd.gyp:clangd',
      ],
    },
    {
      'target_name': 'Tests',
      'type': 'none',
      'dependencies': [
        '../test/test.gyp:unit_tests',
      ],
    },
  ],
}

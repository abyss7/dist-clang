{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'clang_helpers',
      'type': 'static_library',
      'dependencies': [
        '../base/base.gyp:constants',
        '../base/base.gyp:headers_only',
        '../base/base.gyp:logging',
        '../base/base.gyp:process',
        '../net/net.gyp:net',
        '../proto/proto.gyp:proto',
      ],
      'sources': [
        'clang.cc',
        'clang.h',
        'clang_flag_set.cc',
        'clang_flag_set.h',
      ],
    },
    {
      'target_name': 'clang',
      'type': 'executable',
      'dependencies': [
        'clang_helpers',
      ],
      'sources': [
        'clang_main.cc',
      ],
    },
  ],
}

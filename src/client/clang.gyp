{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'client',
      'type': 'shared_library',
      'dependencies': [
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
        '../base/base.gyp:constants',
        '../base/base.gyp:logging',
        'client',
      ],
      'sources': [
        'clang_main.cc',
      ],
    },
  ],
}

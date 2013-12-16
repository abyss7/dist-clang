{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'clangd',
      'type': 'executable',
      'dependencies': [
        '../base/base.gyp:base',
        '../base/base.gyp:constants',
        '../base/base.gyp:hash',
        '../base/base.gyp:process',
        '../net/net.gyp:net',
        '../proto/proto.gyp:proto',
        '../third_party/tclap/tclap.gyp:tclap',
        'file_cache',
      ],
      'sources': [
        'clangd.cc',
        'configuration.cc',
        'configuration.h',
        'daemon.cc',
        'daemon.h',
        'statistic.cc',
        'statistic.h',
      ],
    },
    {
      'target_name': 'file_cache',
      'type': 'static_library',
      'dependencies': [
        '../base/base.gyp:base',
      ],
      'sources': [
        'file_cache.cc',
        'file_cache.h',
      ],
    },
  ],
}

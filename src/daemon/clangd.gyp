{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'clangd',
      'type': 'executable',
      'dependencies': [
        'daemon',
      ],
      'sources': [
        'clangd.cc',
      ],
    },
    {
      'target_name': 'configuration',
      'type': 'shared_library',  # need to be shared - to use exceptions.
      'dependencies': [
        '../base/base.gyp:constants',
        '../base/base.gyp:logging',
        '../build/version.gyp:version',
        '../proto/proto.gyp:proto',
        '../third_party/tclap/tclap.gyp:tclap',
      ],
      'sources': [
        'configuration.cc',
        'configuration.h',
      ],
    },
    {
      'target_name': 'daemon',
      'type': 'static_library',
      'dependencies': [
        '../base/base.gyp:base',
        '../base/base.gyp:constants',
        '../base/base.gyp:hash',
        '../base/base.gyp:logging',
        '../net/net.gyp:net',
        '../proto/proto.gyp:proto',
        'configuration',
        'file_cache',
      ],
      'sources': [
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

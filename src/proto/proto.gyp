{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'proto',
      'type': 'static_library',
      'dependencies': [
        '../third_party/protobuf/protobuf.gyp:protobuf',
      ],
      'direct_dependent_settings': {

      },
      'sources': [
        'base.pb.cc',
        'base.pb.h',
        'base.proto',
        'config.pb.cc',
        'config.pb.h',
        'config.proto',
        'remote.pb.cc',
        'remote.pb.h',
        'remote.proto',
        'utils.h',
      ],
    },
  ],
}

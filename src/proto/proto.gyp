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
      'export_dependent_settings': [
        '../third_party/protobuf/protobuf.gyp:protobuf',
      ],
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

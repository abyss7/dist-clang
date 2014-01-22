{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'proto',
      'type': 'shared_library',
      'dependencies': [
        '../third_party/protobuf/protobuf.gyp:protobuf',
      ],
      'cflags!': [
        '-fno-rtti',
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
        'stats.pb.cc',
        'stats.pb.h',
        'stats.proto',
      ],
    },
  ],
}

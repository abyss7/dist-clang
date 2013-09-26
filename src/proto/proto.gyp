{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'proto',
      'type': 'static_library',
      'direct_dependent_settings': {
        'ldflags': [
         '-lprotobuf',
        ],
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

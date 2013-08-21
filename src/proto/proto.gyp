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
        'remote.pb.cc',
        'remote.pb.h',
        'remote.proto',
      ],
    },
  ],
}

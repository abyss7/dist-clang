{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'base',
      'type': 'static_library',
      'sources': [
        'assert.h',
        'attributes.h',
        'c_utils.h',
        'constants.cc',
        'constants.h',
        'file_utils.h',
        'random.h',
        'read_write_lock.h',
        'string_utils.h',
      ],
    },
    {
      'target_name': 'hash',
      'type': 'static_library',
      'sources': [
        'hash.h',
        'hash/murmur_hash3.cc',
        'hash/murmur_hash3.h',
      ],
    },
    {
      'target_name': 'process',
      'type': 'static_library',
      'dependencies': [
        '../proto/proto.gyp:proto',
      ],
      'sources': [
        'process.cc',
        'process.h',
        'process_linux.cc',
        'process_mac.cc',
      ],
    },
  ],
}

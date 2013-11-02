{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'base',
      'type': 'static_library',
      'dependencies': [
        'headers_only',
      ],
      'sources': [
        'thread_pool.cc',
        'thread_pool.h',
        'worker_pool.cc',
        'worker_pool.h',
      ],
    },
    {
      'target_name': 'constants',
      'type': 'static_library',
      'sources': [
        'constants.cc',
        'constants.h',
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
      'target_name': 'headers_only',
      'type': 'none',
      'sources': [
        'assert.h',
        'attributes.h',
        'c_utils.h',
        'file_utils.h',
        'locked_queue.h',
        'locked_queue_impl.h',
        'queue_aggregator.h',
        'queue_aggregator_impl.h',
        'string_utils.h',
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

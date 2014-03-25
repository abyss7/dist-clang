{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'base',
      'type': 'shared_library',
      'dependencies': [
        'headers_only',
        'logging',
        '../third_party/gtest/gtest.gyp:gtest_headers',
      ],
      'sources': [
        'file_utils.cc',
        'file_utils.h',
        'process.cc',
        'process.h',
        'process_impl.cc',
        'process_impl.h',
        'process_impl_linux.cc',
        'process_impl_mac.cc',
        'temporary_dir.cc',
        'temporary_dir.h',
        'thread_pool.cc',
        'thread_pool.h',
        'worker_pool.cc',
        'worker_pool.h',
      ],
    },
    {
      'target_name': 'constants',
      'type': 'shared_library',
      'sources': [
        'constants.cc',
        'constants.h',
      ],
    },
    {
      'target_name': 'hash',
      'type': 'shared_library',
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
        'empty_lambda.h',
        'future.h',
        'future_impl.h',
        'locked_queue.h',
        'queue_aggregator.h',
        'string_utils.h',
        'testable.h',
      ],
    },
    {
      'target_name': 'logging',
      'type': 'shared_library',
      'sources': [
        'logging.cc',
        'logging.h',
        'using_log.h',
      ],
    },
  ],
}

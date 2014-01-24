{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'net',
      'type': 'shared_library',
      'dependencies': [
        '../base/base.gyp:base',
        '../base/base.gyp:logging',
        '../proto/proto.gyp:proto',
        '../third_party/gtest/gtest.gyp:gtest_headers',
      ],
      'sources': [
        'base/end_point.cc',
        'base/end_point.h',
        'base/types.h',
        'base/utils.h',
        'connection.cc',
        'connection.h',
        'connection_forward.h',
        'connection_impl.cc',
        'connection_impl.h',
        'event_loop.cc',
        'event_loop.h',
        'network_service.h',
        'network_service_impl.cc',
        'network_service_impl.h',
        'network_service_linux.cc',
        'network_service_mac.cc',
      ],
      'conditions': [
        ['OS=="linux"', {
          'sources': [
            'epoll_event_loop.cc',
            'epoll_event_loop.h',
          ],
        }],
        ['OS=="mac"', {
          'sources': [
            'kqueue_event_loop.cc',
            'kqueue_event_loop.h',
          ],
        }],
      ],
    },
  ],
}

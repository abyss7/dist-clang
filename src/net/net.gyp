{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'net',
      'type': 'static_library',
      'dependencies': [
        '../proto/proto.gyp:proto',
      ],
      'sources': [
        'base/end_point.cc',
        'base/end_point.h',
        'base/types.h',
        'base/utils.h',
        'base/worker_pool.cc',
        'base/worker_pool.h',
        'connection.cc',
        'connection.h',
        'connection_forward.h',
        'event_loop.cc',
        'event_loop.h',
        'network_service.cc',
        'network_service.h',
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

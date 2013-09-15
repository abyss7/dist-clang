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
        'base/types.h',
        'base/utils.h',
        'connection.cc',
        'connection.h',
        'connection_forward.h',
        'epoll_event_loop.cc',
        'epoll_event_loop.h',
        'event_loop.cc',
        'event_loop.h',
        'network_service.cc',
        'network_service.h',
      ],
    },
  ],
}

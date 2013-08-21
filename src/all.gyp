{
  'includes': [
    'build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'clangd',
      'type': 'executable',
      'dependencies': [
        'base/base.gyp:base',
        'tclap/tclap.gyp:tclap',
      ],
      'sources': [
        'daemon/clangd.cc',
        'daemon/server.cc',
        'daemon/server.h',
        'daemon/task_queue.cc',
        'daemon/task_queue.h',
        'daemon/thread_pool.cc',
        'daemon/thread_pool.h',
      ],
    },
    {
      'target_name': 'clang',
      'type': 'executable',
      'dependencies': [
        'base/base.gyp:base',
        'proto/proto.gyp:proto',
      ],
      'sources': [
        'client/clang.cc',
        'client/client_tcp.cc',
        'client/client_tcp.h',
        'client/client_unix.cc',
        'client/client_unix.h',
      ],
    },
    {
      'target_name': 'All',
      'type': 'none',
      'dependencies': [
        'clang',
        'clangd',
      ],
    },
  ],
}

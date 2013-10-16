{
  'includes': [
    '../build/defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'clangd',
      'type': 'executable',
      'dependencies': [
        '../base/base.gyp:base',
        '../base/base.gyp:hash',
        '../net/net.gyp:net',
        '../proto/proto.gyp:proto',
        '../third_party/tclap/tclap.gyp:tclap',
      ],
      'sources': [
        'balancer.cc',
        'balancer.h',
        'clangd.cc',
        'command.h',
        'commands/local_execution.cc',
        'commands/local_execution.h',
        'commands/remote_execution.cc',
        'commands/remote_execution.h',
        'configuration.cc',
        'configuration.h',
        'daemon.cc',
        'daemon.h',
        'file_cache.cc',
        'file_cache.h',
        'thread_pool.cc',
        'thread_pool.h',
      ],
    },
  ],
}

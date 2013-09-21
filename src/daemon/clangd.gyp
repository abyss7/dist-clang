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
        '../net/net.gyp:net',
        '../proto/proto.gyp:proto',
        '../tclap/tclap.gyp:tclap',
      ],
      'sources': [
        'balancer.cc',
        'balancer.h',
        'clangd.cc',
        'configuration.cc',
        'configuration.h',
        'daemon.cc',
        'daemon.h',
        'thread_pool.cc',
        'thread_pool.h',
      ],
    },
  ],
}

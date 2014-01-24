{
  'target_defaults': {
    'configurations': {
      'Debug': {
        'cflags': [
          '-fno-exceptions',
          '-g',
          '-O0',
        ],
        'ldflags': [
          '-fno-exceptions',
        ],
      },
      'Release': {
        'cflags': [
          '-fno-exceptions',
          '-fomit-frame-pointer',
          '-O3',
        ],
        'defines': [
          'NDEBUG',
          '_DEBUG',  # for libc++
        ],
        'ldflags': [
          '-fno-exceptions',
        ],
      },
      'Test': {
        'inherit_from': ['Debug'],
        'cflags!': [
          '-fno-exceptions',
        ],
        'ldflags!': [
          '-fno-exceptions',
        ],
      },
    },
    'default_configuration': 'Debug',
  },
}

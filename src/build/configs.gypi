{
  'target_defaults': {
    'configurations': {
      'Debug': {
        'cflags': [
          '-g',
          '-O0',
        ],
        'conditions': [
          ['OS=="linux"', {
            'ldflags': [
              '-rdynamic',  # for backtrace().
            ],
          }],
        ],
      },
      'Release': {
        'cflags': [
          '-fomit-frame-pointer',
          '-O3',
        ],
        'defines': [
          'NDEBUG',
          '_DEBUG',  # for libc++
        ],
        'conditions': [
          ['OS=="linux"', {
            'ldflags': [
              '-rdynamic',  # for backtrace().
            ],
          }],
        ],
      },
    },
    'default_configuration': 'Debug',
  },
}

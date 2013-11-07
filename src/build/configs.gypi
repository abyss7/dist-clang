{
  'target_defaults': {
    'configurations': {
      'Debug': {
        'cflags': [
          '-g',
          '-O0',
        ],
        'ldflags': [
          '-rdynamic',  # for backtrace().
        ],
        'xcode_settings': {
          'OTHER_CFLAGS': [
            '-g',
            '-O0',
          ],
        },
      },
      'Release': {
        'cflags': [
          '-fomit-frame-pointer',
          '-O2',
        ],
        'defines': [
          'NDEBUG',
          '_DEBUG',  # for libc++
        ],
        'ldflags': [
          '-rdynamic',  # for backtrace().
        ],
        'xcode_settings': {
          'OTHER_CFLAGS': [
            '-fomit-frame-pointer',
            '-O2',
          ],
        },
      },
    },
    'default_configuration': 'Debug',
  },
}

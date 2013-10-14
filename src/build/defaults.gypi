{
  'target_defaults': {
    'cflags': [
      '-std=c++11',
      '-pipe',
      '-pthread',
      '-fno-exceptions',
      '-Wall',
      '-Wsign-compare',
      '-Wno-mismatched-tags',  # Remove on libstdc++ >= 4.8.2
      '-Werror',
    ],
    'ldflags': [
      '-ltcmalloc',
    ],
    'xcode_settings': {
      'OTHER_CFLAGS': [
        '-std=c++11',
        '-stdlib=libc++',
        '-pipe',
        '-pthread',
        '-fno-exceptions',
        '-Wall',
        '-Wsign-compare',
        '-Werror',
      ],
    },
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
    'include_dirs': [
      '..',
    ],
  },
}

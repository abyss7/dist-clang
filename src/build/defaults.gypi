{
  'target_defaults': {
    'cflags': [
      '--std=c++11',
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
    'configurations': {
      'Debug': {
        'cflags': [
          '-g',
          '-O0',
        ],
      },
      'Release': {
        'cflags': [
          '-fomit-frame-pointer',
          '-O2',
        ],
        'defines': [
          'NDEBUG',
        ],
      },
    },
    'default_configuration': 'Debug',
    'include_dirs': [
      '..',
    ],
  },
}

{
  'target_defaults': {
    'cflags': [
      '--std=c++11',
      '-pipe',
      '-pthread',
      '-fno-exceptions',
      '-Wall',
      '-Wno-unused-function',
      '-Werror',
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

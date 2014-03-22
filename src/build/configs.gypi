{

  'target_defaults': {
    'xcode_settings': {
      'DYLIB_INSTALL_NAME_BASE': '@rpath',
      'LD_RUNPATH_SEARCH_PATHS': [
        # For unbundled binaries.
        '@loader_path/.',
      ],
    },

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
          '-rpath', '/usr/\$$LIB/dist-clang',
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

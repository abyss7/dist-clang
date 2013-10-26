{
  'variables': {
    'clang%': 0,
    'profiler%': 0,
  },

  'target_defaults': {
    'cflags': [
      '-std=c++11',
      '-pipe',
      '-pthread',
      '-fno-exceptions',
      '-Wall',
      '-Wsign-compare',
      '-Werror',
    ],
    'ldflags': [
      '-ltcmalloc',
    ],
    'xcode_settings': {
      'ARCHS': ['x86_64'],
      'CLANG_CXX_LANGUAGE_STANDARD': 'c++11',
      'CLANG_CXX_LIBRARY': 'libc++',
      'WARNING_CFLAGS': [
        '-Wall',
        '-Wsign-compare',
        '-Werror',
      ],
      'OTHER_CFLAGS': [
        '-pipe',
        '-pthread',
        '-fno-exceptions',
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
    'sources/': [
      ['exclude', '_(linux|mac)\\.cc$'],
    ],
    'conditions': [
      ['OS=="linux"', {
        'defines': [
          'OS_LINUX',
        ],
        'sources/': [
          ['include', '_linux\\.cc$'],
        ],
      }],
      ['OS=="mac"', {
        'defines': [
          'OS_MACOSX',
        ],
        'sources/': [
          ['include', '_mac\\.cc$'],
        ],
      }],
      ['clang==1', {
        'cflags': [
          '-Wno-mismatched-tags',  # Remove on libstdc++ >= 4.8.2
        ],
      }],
      ['profiler==1 and OS=="linux"', {
        'defines': [
          'PROFILER',
        ],
        'ldflags': [
          '-lprofiler',
        ],
      }],
    ],
    'target_conditions': [
      ['_type!="static_library"', {
        'xcode_settings': {
          'OTHER_LDFLAGS': [
            '-lc++',
          ],
        },
      }],
      ['_type=="shared_library"', {
        'cflags': [
          '-fPIC',
        ],
      }],
    ],
  },
}

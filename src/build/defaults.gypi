{
  'variables': {
    'profiler%': 0,
    'tcmalloc%': 1,
  },

  'includes': [
    'configs.gypi',
  ],

  'target_defaults': {
    'cflags': [
      '-fno-rtti',
      '-fPIC',
      '-nostdinc++',
      '-pipe',
      '-pthread',
      '-std=c++11',
      '-Wall',
      '-Wsign-compare',
      '-Werror',
    ],
    'dependencies': [
      '<(DEPTH)/third_party/libcxx/libcxx.gyp:c++',
    ],
    'include_dirs': [
      '..',
    ],
    'ldflags': [
      '-fno-rtti',
      '--no-undefined',
    ],
    'link_settings': {
      'libraries': [
        '-lpthread',
      ],
    },
    'sources/': [
      ['exclude', '_(linux|mac)\\.cc$'],
    ],
    'conditions': [
      ['OS=="linux"', {
        'defines': [
          'OS_LINUX',
        ],
        'dependencies': [
          '<(DEPTH)/third_party/libcxxabi/libcxxabi.gyp:c++abi',
        ],
        'ldflags': [
          '-rdynamic',  # for backtrace().
        ],
        'sources/': [
          ['include', '_linux\\.cc$'],
        ],
      }],
      ['OS=="mac"', {
        'defines': [
          'OS_MACOSX',
        ],
        'ldflags!': [
          '--no-undefined',
        ],
        'sources/': [
          ['include', '_mac\\.cc$'],
        ],
        'xcode_settings': {
          'ARCHS': ['x86_64'],
        },
      }],
      ['profiler==1 and OS=="linux"', {
        'defines': [
          'PROFILER',
        ],
        'link_settings': {
          'libraries': [
            '-lprofiler',
          ],
        },
      }],
      ['tcmalloc==1 and OS=="linux"', {
        'link_settings': {
          'libraries': [
            '-ltcmalloc',
          ],
        },
      }],
    ],
  },
}

{
  'variables': {
    'clang%': 0,
    'profiler%': 0,
    'tcmalloc%': 1,
  },

  'includes': [
    'configs.gypi',
  ],

  'target_defaults': {
    'cflags': [
      '-std=c++11',
      '-stdlib=libc++',
      '-pipe',
      '-pthread',
      '-fno-exceptions',
      '-Wall',
      '-Wsign-compare',
      '-Werror',
    ],
    'ldflags': [
      '-lpthread',
    ],
    'xcode_settings': {
      'ARCHS': ['x86_64'],
    },
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
        'dependencies': [
          '<(DEPTH)/third_party/libcxx/libcxx.gyp:c++',
          '<(DEPTH)/third_party/libcxxabi/libcxxabi.gyp:c++abi',
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
      ['profiler==1 and OS=="linux"', {
        'defines': [
          'PROFILER',
        ],
        'ldflags': [
          '-lprofiler',
        ],
      }],
      ['tcmalloc==1 and OS=="linux"', {
        'ldflags': [
          '-ltcmalloc',
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

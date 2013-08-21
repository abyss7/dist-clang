{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'base',
      'type': 'static_library',
      'sources': [
        'c_utils.h',
        'futex.h',
        'process.cc',
        'process.h',
        'string_utils.h',
      ],
    },
  ],
}

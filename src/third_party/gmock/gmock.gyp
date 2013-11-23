{
  'includes': [
    '../../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'gmock_headers',
      'type': 'none',
      'all_dependent_settings': {
        'include_dirs': [
          '..',
        ],
      },
      'sources': [
        'gmock-actions.h',
        'gmock-cardinalities.h',
        'gmock-generated-actions.h',
        'gmock-generated-function-mockers.h',
        'gmock-generated-matchers.h',
        'gmock-generated-nice-strict.h',
        'gmock.h',
        'gmock-matchers.h',
        'gmock-more-actions.h',
        'gmock-more-matchers.h',
        'gmock-spec-builders.h',
      ],
    },
    {
      'target_name': 'gmock',
      'type': 'shared_library',
      'dependencies': [
        '../gtest/gtest.gyp:gtest',
      ],
      'include_dirs': [
        '..',
      ],
      'sources': [
        'gmock-all.cc',
        'internal/gmock-generated-internal-utils.h',
        'internal/gmock-internal-utils.h',
        'internal/gmock-port.h',
      ],
    },
  ],
}

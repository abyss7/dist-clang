{
  'includes': [
    '../../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'gtest',
      'type': 'shared_library',
      'cflags': [
        '-fPIC',
      ],
      'direct_dependent_settings': {
        'include_dirs': [
          '..',
        ],
      },
      'include_dirs': [
        '..',
      ],
      'sources': [
        'gtest-all.cc',
        'gtest-death-test.h',
        'gtest-internal-inl.h',
        'gtest-message.h',
        'gtest-param-test.h',
        'gtest-printers.h',
        'gtest-spi.h',
        'gtest-test-part.h',
        'gtest-typed-test.h',
        'gtest.h',
        'gtest_pred_impl.h',
        'gtest_prod.h',
        'internal/gtest-death-test-internal.h',
        'internal/gtest-filepath.h',
        'internal/gtest-internal.h',
        'internal/gtest-linked_ptr.h',
        'internal/gtest-param-util-generated.h',
        'internal/gtest-param-util.h',
        'internal/gtest-port.h',
        'internal/gtest-string.h',
        'internal/gtest-tuple.h',
        'internal/gtest-type-util.h',
      ],
    },
  ],
}

{
  'includes': [
    'defaults.gypi',
  ],

  'targets': [
    {
      'target_name': 'All',
      'type': 'none',
      'dependencies': [
        '../client/clang.gyp:clang',
        '../daemon/clangd.gyp:clangd',
      ],
    },

    {
      'target_name': 'Tests',
      'type': 'none',
      'dependencies': [
        '../test/test.gyp:unit_tests',
      ],
    },

    {
      'target_name': 'deb_package',
      'type': 'none',
      'dependencies': [
        'All',
        'version.gyp:version',
      ],
      'copies': [
        {
          'destination': '<(PRODUCT_DIR)/deb/usr/bin/dist-clang',
          'files': [
            '<(PRODUCT_DIR)/clang',
            '<(PRODUCT_DIR)/clangd',
          ],
        },
        {
          'destination': '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang',
          'files': [
            '<(PRODUCT_DIR)/lib/libbase.so',
            '<(PRODUCT_DIR)/lib/libc++.so',
            '<(PRODUCT_DIR)/lib/libc++abi.so',
            '<(PRODUCT_DIR)/lib/libclient.so',
            '<(PRODUCT_DIR)/lib/libconfiguration.so',
            '<(PRODUCT_DIR)/lib/libconstants.so',
            '<(PRODUCT_DIR)/lib/libhash.so',
            '<(PRODUCT_DIR)/lib/liblogging.so',
            '<(PRODUCT_DIR)/lib/libnet.so',
            '<(PRODUCT_DIR)/lib/libproto.so',
          ],
        },
      ],
      'actions': [
        {
          'action_name': 'expand_deb_control',
          'inputs': [
            'deb_control.template',
            'expand_env_vars.sh',
          ],
          'outputs': [
            '<(PRODUCT_DIR)/deb/DEBIAN/control',
          ],
          'action': [
            # TODO: use variable 'version' propagated from target 'version'.
            'env', 'VERSION=<!(git log --oneline | wc -l)',
            'sh', 'expand_env_vars.sh', 'deb_control.template',
            '<(PRODUCT_DIR)/deb/DEBIAN/control',
          ],
        },
        {
          'action_name': 'create_deb_package',
          'inputs': [
            '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang',
            '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clangd',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libbase.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libc++.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libc++abi.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libclient.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libconfiguration.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libconstants.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libhash.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/liblogging.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libnet.so',
            '<(PRODUCT_DIR)/deb/usr/lib64/dist-clang/libproto.so',
            '<(PRODUCT_DIR)/deb/DEBIAN/control',
          ],
          'outputs': [
            '<(PRODUCT_DIR)/dist-clang.deb',
          ],
          'action': [
            'dpkg-deb', '-z9', '-Zxz', '-b', '<(PRODUCT_DIR)/deb',
            '<(PRODUCT_DIR)/dist-clang_<!(git log --oneline | wc -l)_amd64.deb',
          ],
        },
      ],
    },
  ],
}

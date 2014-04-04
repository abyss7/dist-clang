{
  'includes': [
    'defaults.gypi',
    'version.gypi',
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
  ],

  'conditions': [
    ['OS=="linux"', {
        'targets': [
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
                'clangd_wrapper',
                '<(PRODUCT_DIR)/clang',
                '<(PRODUCT_DIR)/clangd',
              ],
            },
            {
              'destination': '<(PRODUCT_DIR)/deb/usr/lib/dist-clang',
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
            {
              'destination': '<(PRODUCT_DIR)/deb/etc',
              'files': [
                'clangd.conf',
              ],
            },
          ],
          'actions': [
            {
              'action_name': 'copy_init_d',
              'inputs': [
                'clangd_init_d',
              ],
              'outputs': [
                '<(PRODUCT_DIR)/deb/etc/init.d/clangd',
              ],
              'action': [
                'cp', '<@(_inputs)', '<@(_outputs)',
              ],
            },
            {
              'action_name': 'clang++_symlink',
              'inputs': [
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang',
              ],
              'outputs': [
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang++',
              ],
              'action': [
                'ln', '-fs', 'clang',
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang++',
              ],
            },
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
                'env', 'VERSION=<(version)',
                'sh', 'expand_env_vars.sh', 'deb_control.template',
                '<@(_outputs)',
              ],
            },
            {
              'action_name': 'create_deb_package',
              'inputs': [
                '<(PRODUCT_DIR)/deb/etc/clangd.conf',
                '<(PRODUCT_DIR)/deb/etc/init.d/clangd',
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang',
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clang++',
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clangd',
                '<(PRODUCT_DIR)/deb/usr/bin/dist-clang/clangd_wrapper',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libbase.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libc++.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libc++abi.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libclient.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libconfiguration.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libconstants.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libhash.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/liblogging.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libnet.so',
                '<(PRODUCT_DIR)/deb/usr/lib/dist-clang/libproto.so',
                '<(PRODUCT_DIR)/deb/DEBIAN/control',
              ],
              'outputs': [
                '<(PRODUCT_DIR)/dist-clang.deb',
              ],
              'action': [
                'dpkg-deb', '-z9', '-Zxz', '-b', '<(PRODUCT_DIR)/deb',
                '<(PRODUCT_DIR)/dist-clang_<(version)_amd64.deb',
              ],
            },
          ],
        },
      ],
    }]
  ],
}

{
  'includes': [
    'configs.gypi',
    'version.gypi',
  ],

  'targets': [
    {
      'target_name': 'version',
      'type': 'none',
      'direct_dependent_settings': {
        'defines': [
          'VERSION="<(version)"',
        ],
      },
    },
  ],
}

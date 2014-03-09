{
  'includes': [
    'configs.gypi',
  ],

  'targets': [
    {
      'target_name': 'version',
      'type': 'none',
      'direct_dependent_settings': {
        'variables': {
          'version%': '<!(git log --oneline | wc -l)',
        },
        'defines': [
          'VERSION="<(version)"',
        ],
      },
    },
  ],
}

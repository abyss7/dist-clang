{
  'variables': {
    'variables': {
      'version%': 0,
    },
    'version%': "<(version)",
    'conditions': [
      ['version != 0', {
        # version is defined through GYP_DEFINES
      }, {
        'version': '<!(git log --oneline | wc -l)',
      }]
    ],
  },
}

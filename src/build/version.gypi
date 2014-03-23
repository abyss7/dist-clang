{
  'variables': {
    'version%': 0,
  },

  'conditions': [
    ['version!=0', {
      # version is defined through GYP_DEFINES
    }, {
      'variables': {
        'version%': '<!(git log --oneline | wc -l)',
      },
    }]
  ],
}

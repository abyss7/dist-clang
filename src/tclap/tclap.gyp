{
  'includes': [
    '../build/defaults.gypi',
  ],
  'targets': [
    {
      'target_name': 'tclap',
      'type': 'none',
      'direct_dependent_settings': {
        'cflags!': [
          '-fno-exceptions',  # TCLAP uses exceptions.
        ],
      },
      'sources': [
        'Arg.h',
        'ArgException.h',
        'ArgTraits.h',
        'CmdLine.h',
        'CmdLineInterface.h',
        'CmdLineOutput.h',
        'Constraint.h',
        'DocBookOutput.h',
        'HelpVisitor.h',
        'IgnoreRestVisitor.h',
        'MultiArg.h',
        'MultiSwitchArg.h',
        'OptionalUnlabeledTracker.h',
        'StandardTraits.h',
        'StdOutput.h',
        'SwitchArg.h',
        'UnlabeledMultiArg.h',
        'UnlabeledValueArg.h',
        'ValueArg.h',
        'ValuesConstraint.h',
        'VersionVisitor.h',
        'Visitor.h',
        'XorHandler.h',
        'ZshCompletionOutput.h',
      ],
    },
  ],
}

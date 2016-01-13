#!/usr/bin/env python
# -*- coding: utf-8 -*-

#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import subprocess
import sys

# Hack for running in test configuration.
env = os.environ
env['LSAN_OPTIONS'] = 'detect_leaks=0'

subprocess.Popen(['./llvm-tblgen'] + sys.argv[1:], env=env)

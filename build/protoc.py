#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import subprocess
import sys

# Hack for running 'protoc' in test configuration.
env = os.environ
env['LSAN_OPTIONS'] = 'detect_leaks=0'

subprocess.Popen(['./protoc', '--proto_path=' + sys.argv[1],
                  '--cpp_out=' + sys.argv[2],
                  '--python_out=' + sys.argv[2], sys.argv[3]], env=env)

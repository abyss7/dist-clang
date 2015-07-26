#!/usr/bin/env python
# -*- coding: utf-8 -*-

import subprocess
import sys

subprocess.check_call(['protoc', '--proto_path=' + sys.argv[1],
                       '--cpp_out=' + sys.argv[2], sys.argv[3]])

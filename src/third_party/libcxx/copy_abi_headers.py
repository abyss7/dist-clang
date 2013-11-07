# -*- coding: utf-8 -*-

import os
import shutil
import subprocess
import sys

command = \
    'echo | ' \
    'gcc -Wp,-v -x c++ - -fsyntax-only 2>&1 >/dev/null | ' \
    'grep "^ " | ' \
    'sed "s/^\( \)//g"'
abi_headers = [
    '/bits/c++config.h',
    '/bits/cpu_defines.h',
    '/bits/cxxabi_forced.h',
    '/bits/cxxabi_tweaks.h',
    '/bits/os_defines.h',
    '/cxxabi.h',
]

try:
    os.makedirs(sys.argv[1] + '/bits')
except:
    pass
process = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE)
for line in process.stdout.readlines():
    for header in abi_headers:
        try:
            shutil.copy(line.split()[0] + header, sys.argv[1] + header)
            print "Found", header
        except:
            pass
result = process.wait()

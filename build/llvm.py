import subprocess
import sys

argv = sys.argv[1:]

if argv[0] == "defines":
  flags = subprocess.check_output(['llvm-config', '--cxxflags']).strip().split()
  for flag in flags:
    if flag.startswith('-D'):
      print flag[2:]

elif argv[0] == "libs":
  flags = subprocess.check_output(['llvm-config', '--libs']).strip().split()
  for flag in flags:
    if flag.startswith('-l'):
      print flag[2:]

else:
  flags = subprocess.check_output(['llvm-config', '--' + argv[0]]).strip().split()
  for flag in flags:
    print flag


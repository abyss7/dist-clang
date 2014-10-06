import subprocess

lines = subprocess.check_output(['xcodebuild', '-sdk', '-version']).splitlines()
parts = lines[2].split(': ')
print "-L" + parts[1] + "/usr/lib/system"

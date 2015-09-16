import os
import subprocess
import sys

dirname = os.path.dirname(os.path.abspath(__file__))
merge_base = subprocess.check_output("git -C {} merge-base HEAD master".format(dirname), shell=True).split()[0]
major = len(subprocess.check_output("git -C {} log --oneline {}".format(dirname, merge_base), shell=True).splitlines())

if subprocess.check_output("git -C {} rev-parse HEAD".format(dirname), shell=True).split()[0] == merge_base:
    sys.stdout.write(str(major) + ".0")

minor = len(subprocess.check_output("git -C {} log --oneline {}..HEAD".format(dirname, merge_base), shell=True).splitlines())
sys.stdout.write(str(major) + "." + str(minor))

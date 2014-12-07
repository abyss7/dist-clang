import os
import subprocess

dirname = os.path.dirname(os.path.abspath(__file__))
print len(subprocess.check_output("git -C {} log --oneline master".format(dirname), shell=True).splitlines())

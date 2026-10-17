import os
import subprocess
import sys

dirname = os.path.dirname(os.path.abspath(__file__))


def Git(args):
    # For both Python 2 and 3.
    with open(os.devnull, "w") as devnull:
        return subprocess.check_output("git -C {} {}".format(dirname, args), shell=True,
                                       stderr=devnull, universal_newlines=True)


def MergeBase():
    # The clones of CI have only the remote branch.
    for branch in ("master", "origin/master"):
        try:
            return Git("merge-base HEAD {}".format(branch)).split()[0]
        except subprocess.CalledProcessError:
            pass
    # E.g. the shallow clone.
    sys.stdout.write("0.0")
    sys.exit(0)


merge_base = MergeBase()
major = len(Git("log --oneline {}".format(merge_base)).splitlines())

if Git("rev-parse HEAD").split()[0] == merge_base:
    sys.stdout.write(str(major) + ".0")
else:
    minor = len(Git("log --oneline {}..HEAD".format(merge_base)).splitlines())
    sys.stdout.write(str(major) + "." + str(minor))

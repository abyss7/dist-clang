import os
import subprocess
import sys


dirname = os.path.dirname(os.path.abspath(__file__))


def git_output(*cmd):
    return subprocess.check_output(("git", "-C", dirname) + cmd)


def has_master():
    proc = subprocess.Popen(["git", "-C", dirname, "rev-parse", "master"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    proc.communicate()
    return proc.returncode == 0


if has_master():
    merge_base = git_output("merge-base", "HEAD", "master").split()[0]
    major = len(git_output("log", "--oneline", merge_base).splitlines())

    if git_output("rev-parse", "HEAD").split()[0] == merge_base:
        sys.stdout.write(str(major) + ".0")
    else:
        minor = len(git_output(
            "log", "--oneline", "{}..HEAD".format(merge_base)).splitlines())
        sys.stdout.write(str(major) + "." + str(minor))
else:
    sys.stdout.write("unknown")

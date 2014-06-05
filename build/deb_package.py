import os
import shutil
import subprocess
import sys

argv = sys.argv[1:]
product_dir = argv[0]
version = argv[1]
top_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

execfile(os.path.join(top_dir, "build", "common_package.include"))
MakeInstall(top_dir, product_dir, "deb")

# Create Debian-control file
args = ['sh']
args.append(os.path.join(top_dir, "build", "expand_env_vars.sh"))
args.append(os.path.join(top_dir, "build", "deb_control.template"))
args.append(os.path.join(product_dir, "deb", "DEBIAN", "control"))
subprocess.Popen(args, env = {'VERSION': version}).wait()

# Create .deb file
args = ['dpkg-deb', '-z9', '-Zxz', '-b']
args.append(os.path.join(product_dir, "deb"))
args.append(os.path.join(product_dir, "dist-clang_{}_amd64.deb".format(version)))
subprocess.Popen(args).wait()

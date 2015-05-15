import os
import shutil
import subprocess
import sys
import time

argv = sys.argv[1:]
product_dir = argv[0]
version = argv[1]
top_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

# Environment variables
date = time.strftime("%a, %d %b %Y %H:%M:%S")
package_name = "dist-clang_{}_amd64".format(version)
env_vars = {'VERSION': version, 'DATE': date,
            'PACKAGE_NAME': package_name + '.deb'}

execfile(os.path.join(top_dir, "build", "common_package.include"))
MakeInstall(top_dir, product_dir, os.path.join("deb", "debian", "tmp"))

# Debian specific install.
supervisor_dir = os.path.join(
    product_dir, "deb", "debian", "tmp", "etc", "supervisor", "conf.d")
os.makedirs(supervisor_dir)
shutil.copy(os.path.join(top_dir, "install", "supervisord_service"),
            os.path.join(supervisor_dir, "clangd.conf"))

# Create 'control' file
deb_control = os.path.join(product_dir, "deb", "debian", "control")
if os.path.isfile(deb_control) and os.access(deb_control, os.R_OK):
    os.remove(deb_control)
args = ['sh']
args.append(os.path.join(top_dir, "build", "expand_env_vars.sh"))
args.append(os.path.join(top_dir, "build", "deb_control.template"))
args.append(os.path.join(product_dir, "deb", "debian", "control"))
subprocess.Popen(args, env=env_vars).wait()

# Create 'changelog' file
deb_changelog = os.path.join(product_dir, "deb", "debian", "changelog")
if os.path.isfile(deb_changelog) and os.access(deb_changelog, os.R_OK):
    os.remove(deb_changelog)
args = ['sh']
args.append(os.path.join(top_dir, "build", "expand_env_vars.sh"))
args.append(os.path.join(top_dir, "build", "deb_changelog.template"))
args.append(os.path.join(product_dir, "deb", "debian", "changelog"))
subprocess.Popen(args, env=env_vars).wait()

# Create 'DEBIAN/control' file
deb_files = os.path.join(product_dir, "deb", "debian", "files")
if os.path.isfile(deb_files) and os.access(deb_files, os.R_OK):
    os.remove(deb_files)
os.chdir(os.path.join(product_dir, "deb"))
os.makedirs(os.path.join(product_dir, "deb", "debian", "tmp", "DEBIAN"))
args = ['dpkg-gencontrol']
subprocess.Popen(args).wait()

# Copy 'DEBIAN/prerm' file
shutil.copy(os.path.join(top_dir, "install", "debian_prerm"),
            os.path.join(product_dir, "deb", "debian", "tmp", "DEBIAN", "prerm"))

# Create .deb file
args = ['fakeroot', 'dpkg-deb', '-z9', '-Zxz', '-b']
args.append(os.path.join(product_dir, "deb", "debian", "tmp"))
args.append(os.path.join(product_dir, package_name + '.deb'))
subprocess.Popen(args).wait()

# Create .changes file
changes_file = open(os.path.join(product_dir, package_name + '.changes'), "w")
args = ['dpkg-genchanges', '-b']
subprocess.Popen(args, stdout=changes_file).wait()
changes_file.close()

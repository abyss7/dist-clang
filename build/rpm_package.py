import os
import shutil
import subprocess
import sys

argv = sys.argv[1:]
product_dir = argv[0]
version = argv[1]
top_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

execfile(os.path.join(top_dir, "build", "common_package.include"))
MakeInstall(top_dir, product_dir, "rpm")

# RPM specific install.
systemd_dir = os.path.join(
    product_dir, "rpm", "usr", "lib", "systemd", "system")
os.makedirs(systemd_dir)
shutil.copy(os.path.join(top_dir, "install", "systemd_service"),
            os.path.join(systemd_dir, "clangd.service"))

# Create RPM-spec file
args = ['sh']
args.append(os.path.join(top_dir, "build", "expand_env_vars.sh"))
args.append(os.path.join(top_dir, "build", "rpm_spec.template"))
args.append(os.path.join(product_dir, "rpm.spec"))
subprocess.Popen(args, env={'VERSION': version}).wait()

# Setup rpmbuild top directory
shutil.rmtree(os.path.join(product_dir, "rpmbuild"))

# Create .rpm file
args = ['rpmbuild', '-bb', '--buildroot']
args.append(os.path.join(product_dir, "rpm"))
args.append('--define')
args.append('_topdir ' + os.path.join(product_dir, "rpmbuild"))
args.append('--target')
args.append('x86_64')
args.append(os.path.join(product_dir, "rpm.spec"))
subprocess.Popen(args).wait()

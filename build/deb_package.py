import os
import shutil
import subprocess
import sys

argv = sys.argv[1:]
product_dir = argv[0]
version = argv[1]
top_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

# Cleanup tree
bin_dir = os.path.join(product_dir, "deb", "usr", "bin", "dist-clang")
lib_dir = os.path.join(product_dir, "deb", "usr", "lib", "dist-clang")
etc_dir = os.path.join(product_dir, "deb", "etc")
init_dir = os.path.join(etc_dir, "init.d")
shutil.rmtree(os.path.join(product_dir, "deb"))
os.makedirs(bin_dir)
os.makedirs(lib_dir)
os.makedirs(etc_dir)
os.makedirs(init_dir)

# Copy executables
shutil.copy(os.path.join(top_dir, "install", "clangd_wrapper"), bin_dir)
shutil.copy(os.path.join(product_dir, "clang"), bin_dir)
shutil.copy(os.path.join(product_dir, "clangd"), bin_dir)
os.symlink("clang", os.path.join(bin_dir, "clang++"))

# Copy libraries
shutil.copy(os.path.join(product_dir, "lib", "libbase.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libc++.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libconfiguration.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libconstants.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libhash.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "liblogging.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libnet.so"), lib_dir)
shutil.copy(os.path.join(product_dir, "lib", "libproto.so"), lib_dir)

# Copy configs
shutil.copy(os.path.join(top_dir, "install", "clangd.conf"), etc_dir)
shutil.copy(os.path.join(top_dir, "install", "clangd_init_d"), os.path.join(init_dir, "clangd"))

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

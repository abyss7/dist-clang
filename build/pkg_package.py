import os
import shutil
import subprocess
import sys

argv = sys.argv[1:]
product_dir = argv[0]
version = argv[1]
top_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

execfile(os.path.join(top_dir, "build", "common_package.include"))
MakeInstall(top_dir, product_dir, "pkg")

# PKG specific install.
launchd_dir = os.path.join(product_dir, "pkg", "Library", "LaunchDaemons")
os.makedirs(launchd_dir)
shutil.copy(os.path.join(top_dir, "install", "launchd_service"),
            os.path.join(launchd_dir, "ru.yandex.clangd.plist"))

# Install conf file for newsyslog on mac
newsyslog_dir = os.path.join(product_dir, "pkg", "etc", "newsyslog.d")
os.makedirs(newsyslog_dir)
shutil.copy(os.path.join(top_dir, "install", "ru.yandex.clangd.conf"),
            newsyslog_dir)

# Check plist with linter.
subprocess.call(
    ['plutil', '-lint', os.path.join(launchd_dir, "ru.yandex.clangd.plist")])

# Create makefile
shutil.copy(os.path.join(top_dir, "build", "luggage_makefile.template"),
            os.path.join(product_dir, "Makefile"))

# Create .pkg file.
args = ["fakeroot", "make", "pkg"]
env = os.environ.copy()
env.update({'VERSION': version, 'PROD_DIR': product_dir})
subprocess.Popen(args, env=env).wait()

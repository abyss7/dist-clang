#!/usr/bin/env bash

set -ex

# Trusty's clang has no 3.6 version, so make symlinks ourselves.
sudo ln -s /usr/bin/clang-3.6 /usr/bin/clang
sudo ln -s /usr/bin/clang++-3.6 /usr/bin/clang++

base_dir="$(dirname $0)"

"$base_dir/configure"
ninja -C "$base_dir/out/Debug.gn" All

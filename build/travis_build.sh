#!/usr/bin/env bash

set -ex

# Trusty's clang package has no 3.8 version, so make symlinks ourselves.
for binary in clang clang++ llvm-symbolizer; do
    sudo ln -s "/usr/bin/${binary}-3.8" "/usr/bin/${binary}"
done

build_dir="$(dirname $0)"

"$build_dir/configure"

# Use system clang for the build.
PATH=/usr/bin:$PATH ninja -C "$build_dir/../out/Test.gn" Tests

"$build_dir/run_all_tests" --test-launcher-bot-mode

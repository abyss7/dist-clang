#!/usr/bin/env bash

set -ex

root_dir="$(readlink -e $(dirname $0)/..)"

"$root_dir/build/configure"

clang_revision=268813-1
clang_root="$root_dir/out/clang-$clang_revision"
if ! test -d "$clang_root"; then
    mkdir "$clang_root"
    base_url=https://commondatastorage.googleapis.com/chromium-browser-clang
    wget -O- "$base_url/Linux_x64/clang-${clang_revision}.tgz" |
        tar -C "$clang_root" -xzf -
fi

PATH=$clang_root/bin:$PATH ninja -C "$root_dir/out/Test.gn" Tests

"$root_dir/build/run_all_tests" --test-launcher-bot-mode

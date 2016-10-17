#!/usr/bin/env bash

set -ex

root_dir="$(cd "$(dirname $0)/.." && pwd -P)"

"$root_dir/build/configure"

if [[ "$(uname)" == Linux ]]; then
    clang_revision=268813-1
    clang_root="$root_dir/out/clang-$clang_revision"
    if ! test -d "$clang_root"; then
        mkdir "$clang_root"
        base_url=https://commondatastorage.googleapis.com/chromium-browser-clang
        curl "$base_url/Linux_x64/clang-${clang_revision}.tgz" |
            tar -C "$clang_root" -xzf -
    fi
    export PATH=$clang_root/bin:$PATH
fi

ninja -C "$root_dir/out/Test.gn" Tests

"$root_dir/build/run_all_tests" --test-launcher-bot-mode

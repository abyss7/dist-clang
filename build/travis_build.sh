#!/usr/bin/env bash

set -ex

root_dir="$(cd "$(dirname $0)/.." && pwd -P)"

"$root_dir/build/configure"

clang_revision=268813-1
clang_root="$root_dir/out/clang-$clang_revision"
if ! test -d "$clang_root"; then
    mkdir "$clang_root"
    if [[ "$(uname)" == Linux ]]; then
        platform=Linux_x64
    elif [[ "$(uname)" == Darwin ]]; then
        platform=Mac
    else
        echo "unknown clang platform '$(uname)'" 1>&2
        exit 1
    fi
    base_url=https://commondatastorage.googleapis.com/chromium-browser-clang
    curl "$base_url/$platform/clang-${clang_revision}.tgz" |
        tar -C "$clang_root" -xzf -
fi

PATH=$clang_root/bin:$PATH ninja -C "$root_dir/out/Test.gn" Tests

"$root_dir/build/run_all_tests" --test-launcher-bot-mode

#!/usr/bin/env bash

set -e

root_dir="$(cd "$(dirname $0)/.." && pwd -P)"

# gn and ninja are extracted into build dir
export PATH="$PATH:$TRAVIS_BUILD_DIR"

"$root_dir/build/configure"

git -C "$root_dir" fetch origin master
ninja -C "$root_dir/out/Debug.gn" All
ninja -C "$root_dir/out/Release.gn" All # TODO: also build 'Packages'
ninja -C "$root_dir/out/Portable.gn" All
ninja -C "$root_dir/out/Test.gn" Tests

"$root_dir/build/run_all_tests" --test-launcher-bot-mode

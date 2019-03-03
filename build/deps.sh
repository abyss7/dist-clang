#!/usr/bin/env bash

set -e

if [[ "$TRAVIS_OS_NAME" == "linux" ]]; then
    curl -JLO https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/T09VSJ7uJnfkIHG3v7o0oYjo0nWLwvFnR05-I9iRU-EC
    unzip -q gn-linux-amd64.zip
    curl -JLO https://github.com/ninja-build/ninja/releases/download/v1.8.2/ninja-linux.zip
    unzip -q ninja-linux.zip
fi

if [[ "$TRAVIS_OS_NAME" == "osx" ]]; then
    curl -JLO https://chrome-infra-packages.appspot.com/dl/gn/gn/mac-amd64/+/EDk770Anp4Qo6I8K2CaZA-aJcEjZ6pAqD7QSqcoEuIMC
    unzip -q gn-mac-amd64.zip
    curl -JLO https://github.com/ninja-build/ninja/releases/download/v1.8.2/ninja-mac.zip
    unzip -q ninja-mac.zip
fi

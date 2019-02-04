#!/usr/bin/env bash

export PS4="☢"

set -ex

if [[ "$TRAVIS_OS_NAME" == "linux" ]]; then
    curl -JLO https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/T09VSJ7uJnfkIHG3v7o0oYjo0nWLwvFnR05-I9iRU-EC
    unzip -q gn-linux-amd64.zip
fi

if [[ "$TRAVIS_OS_NAME" == "osx" ]]; then
    curl -JLO https://chrome-infra-packages.appspot.com/dl/gn/gn/mac-amd64/+/EDk770Anp4Qo6I8K2CaZA-aJcEjZ6pAqD7QSqcoEuIMC
    unzip -q gn-mac-amd64.zip
fi

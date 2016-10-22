[![Build Status](https://travis-ci.org/abyss7/dist-clang.svg?branch=master)](https://travis-ci.org/abyss7/dist-clang)

**DistClang** is the Clang compiler extension with a client-server infrastructure. It features the distributed cross-platform compilation and the intermediate result caching.

The project consists of 2 executables and a couple of configuration files.

**clang** is a client part and should reside on the machine where the compilation is invoked. It should replace the invocation of the original compiler.

**clangd** is a server that has 2 different roles: **emitter** and **absorber**.

# How to build

First of all do clone with an argument `--recurse-submodules` and then configure the project:

    ./build/configure
    
To build You have to use a recent Clang compiler with C++14 support.

## For debugging and local usage

    ninja -C out/Debug.gn All
    cd out/Debug.gn
    ln -s clang clang++

The resulting files `clang`, `clang++` and `clangd` are located in the `out/Debug.gn` folder.

## Linux DEB and RPM packages

    ninja -C out/Release.gn rpm_package deb_package

The resulting packages are:

    out/Release.gn/dist-clang_<version>_amd64.deb
    out/Release.gn/rpmbuild/RPMS/x86_64/dist-clang-<version>-1.x86_64.rpm

Don't use locally the `clang` and `clangd` from the `out/Release.gn` folder since they are hardcoded to use libraries from `/usr/lib/dist-clang` folder.

## Mac OS X package

    ninja -C out/Release.gn pkg_package

The resulting package is `out/Release.gn/dist-clang-<version>.pkg`

# How to configure the emitter

TODO!

# How to configure the absorber

TODO!

# How to run local compilation

The basics is to make use of dist-clang's `clang` and `clang++` as the compilers. Doing

    export CC=/usr/bin/dist-clang/clang CXX=/usr/bin/dist-clang/clang++

possibly should work almost always.

To work properly the dist-clang should know about the real compiler's path and compiler's version.

## Use local config file

One way to provide information about a real compiler is to put config file somewhere on the path to the folder where the build is performed. File must be named `.distclang` and should contain something like this:

    path: "third_party/llvm-build/Release+Asserts/bin/clang"
    version: "clang version 3.7.0 (trunk 231690)"

## Use environment variables

Another way - is to set some env. vars:

    export DC_CLANG_PATH="/usr/bin/clang"
    export DC_CLANG_VERSION="clang version 3.7.0 (trunk 231690)"

## Rely on auto-detect

The last resort is to use dist-clang's auto-detect feature: it tries to find the next `clang` in the path, that differs from the current binary, i.e. `/usr/bin/dist-clang/clang`. It's a not recommended and error-prone way, since internally paths are compared as a raw strings - without link resolution, etc.

In any way, if the clang path is provided without version, then the version is carved out of the real clang's output.

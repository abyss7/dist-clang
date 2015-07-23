*README is under construction!*

---

# How to build

The common part is to make a checkout and to configure the dist-clang setup:

    git clone --recurse-submodules <dist-clang.git> dist-clang
    cd dist-clang
    ./build/configure

## For debugging and local usage

    ninja -C out/Debug.gn All
    cd out/Debug.gn
    ln -s clang clang++
    
The resulting files `clang`, `clang++` and `clangd` are located in the `out/Debug.gn` folder.

## For Linux deployment

    ninja -C out/Release.gn rpm_package deb_package

The resulting packages are:

    out/Release.gn/dist-clang_<version>_amd64.deb
    out/Release.gn/rpmbuild/RPMS/x86_64/dist-clang-<version>-1.x86_64.rpm

Don't use the `clang` and `clangd` from the `out/Release.gn` folder since they are hardcoded to use libraries from `/usr/lib/dist-clang` folder.

## For Mac OS X deployment

    ninja -C out/Release.gn pkg_package
    
The resulting package is `out/Release.gn/dist-clang-<version>.pkg`

Both on Mac OS X and Linux the packages install files in the following folders:
  - /etc
  - /usr/bin/dist-clang/
  - /usr/lib/dist-clang/
  - /usr/lib/python2.7/dist_clang/

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

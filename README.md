*README is under construction!*

---

# How to build

The common part is checkout and configure the dist-clang setup:

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

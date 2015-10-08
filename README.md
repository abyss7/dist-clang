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

Packages will install files into the following folders:
  - /etc
  - /usr/bin/dist-clang/
  - /usr/lib/dist-clang/
  - /usr/lib/python2.7/dist\_clang/

## For Mac OS X deployment

    ninja -C out/Release.gn pkg_package
    
The resulting package is `out/Release.gn/dist-clang-<version>.pkg`

Package will install files into  the following folders:
  - /etc
  - /usr/local/bin/dist-clang/
  - /usr/local/lib/dist-clang/
  - /usr/local/lib/python2.7/dist\_clang/

# Configuration

Configuration file of dist-clang is located in the `/etc/clangd.conf`
It also can be provided with `-f` command line argument:

```bash
/usr/bin/dist-clang/clangd -f my-clangd.conf
```

Dist clang must be configured either as emmiter or as absorber.

## Common configuration

Provide path and size (in bytes) for cache:

```
cache {
  path: "~/.cache/clangd"
  size: 32212254720
}
```

Set logging verbosity:

```
verbosity {
  levers {
    right: 30
    left: 0
  }
}
```

## Emmitter configuration

Set uid of user which will use dist-clang. It can be get by
`id -u` command.

```
user_id: 1000
```

Configure emmiter itself. This section must contain information about
each remote host used for compilation.

```
emitter {
  # Path to unix socket that will be used for communication with daemon.
  socket_path: "/tmp/clangd.socket"
  # Number of local build threads.
  threads: 8
  # Set this setting to true if you want to build on emitter only files that
  # failed to compile on remote host
  only_failed: false

  # This section must be repeated for each remote host.
  remotes {
    host: "distclang-1.example.com"
    # Set to true to communicate with remote host over IPv6.
    ipv6: true
    # Number of compilation threads to run on this remote host.
    # Optional, 2 by default.
    threads: 2
  }

  remotes {
    host: "distclang-2.example.com"
    ipv6: true
    threads: 2
  }

  remotes {
    host: "distclang-3.example.com"
    ipv6: true
    threads: 2
  }
}
```

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

# cppblog
A simple pure backend blog renderer written in C++

## Features
- Single executable file
- No runtime overhead
- Low memory overhead (minimum 4.5MB running with -O3 optimization)
- No embedded js and css
- Full logging support
- Hot reload support
- Pure backend rendering
## Build
Requires:
- git
- cmake 3.10 or later
- ninja-build or GNU Make
- coreutils
- C++ compiler supported C++17

run command to build it:
```bash
$ mkdir build
$ cdbuild
$ cmake .. -GNinja
$ ninja install
```

## Third party libraries
see include folder for more details

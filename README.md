# freedv-backend

This repository contains a series of static libraries used by other FreeDV repositories (such as [freedv-integrations](https://github.com/tmiw/freedv-integrations)) to support various functions:

1. RADE encode and decode.
2. Audio processing (e.g. resampling, AGC and RNNoise, among other actions).
3. FreeDV Reporter/PSK Reporter handling (TCP/IP, callsign encoding/decoding in RADE transmissions)

This is intended to be used as a Git submodule, i.e.

```
project/src$ git submodule add https://github.com/tmiw/freedv-backend backend
...

# inside src/CMakeLists.txt
add_subdirectory(backend)
```

with required libraries linked in as needed.

## Compiling standalone

This project can be compiled standalone by running `cmake`:

```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

To enable unit tests, you can pass `-DUNITTEST=1` to `cmake`:

```
$ cmake -DUNITTEST=1 ..
$ make
$ ctest -V
```

It's highly recommended to also set up a Python venv so that the "loss" tests can be executed
(see [RADE README](https://github.com/drowe67/radae/blob/main/README.md#verifying-rade-integration)):

```
$ python3 -m venv rade-venv
$ . ./rade-venv/bin/activate
$ pip install torch matplotlib
$ mkdir build
$ cd build
$ cmake -DRADAE_PYTHON_EXECUTABLE=$(pwd)/../rade-venv/bin/python3 -DUNITTEST=1 ..
$ make
$ ctest -V
```

## Getting Support

Please create a GitHub issue if you find a problem with this repository.

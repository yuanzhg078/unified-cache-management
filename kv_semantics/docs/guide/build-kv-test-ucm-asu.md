# KV-Test and UCM ASU Build Guide

This guide covers the current build paths for KV-Test and UCM ASU on Ascend.

## Build KV-Test only with the FAKE provider

This does not build or install the UCM Python package.

```bash
cmake -S kv_semantics -B build-kv-test \
  -DCMAKE_BUILD_TYPE=Release \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_UNIT_TESTS=OFF \
  -DBUILD_KV_CLIENT_PROVIDER_FAKE=ON \
  -DBUILD_KV_CLIENT_PROVIDER_AICPU=OFF \
  -DBUILD_KV_CLIENT_PROVIDER_AIV=OFF

cmake --build build-kv-test --target kv-test -j"$(nproc)"
```

The executable is normally located at:

```bash
./build-kv-test/kv_test/kv-test
```

For example:

```bash
./build-kv-test/kv_test/kv-test bench \
  --config ./kv_semantics/kv_test/kv_test.conf
```

The selected runtime configuration must use the FAKE provider:

```ini
transport.provider_type=FAKE
metrics.enabled=true
```

After the first configuration, incremental builds only need:

```bash
cmake --build build-kv-test --target kv-test -j"$(nproc)"
```

## Build and editable-install UCM ASU with FAKE

Use the Python installation entry point. `setup.py` translates `PLATFORM=ascend`
into `-DRUNTIME_ENVIRONMENT=ascend`, and `BUILD_UCM_ASU=1` into
`-DBUILD_UCM_ASU=ON`.

```bash
export PLATFORM=ascend
export BUILD_UCM_ASU=1

python -m pip install -v -e . --no-build-isolation
```

FAKE is enabled by the CMake default:

```cmake
option(BUILD_KV_CLIENT_PROVIDER_FAKE "Build KV fake trans provider" ON)
```

AICPU and AIV are OFF by default, so they do not need to be set to `0` in a
fresh CMake build cache.

This path currently compiles the `kv-test` executable because
`kv_semantics/CMakeLists.txt` unconditionally includes the `kv_test`
subdirectory. It does not enable or run unit tests in a fresh build cache.

## Build and editable-install UCM ASU with AICPU

```bash
export PLATFORM=ascend
export BUILD_UCM_ASU=1
export BUILD_KV_CLIENT_PROVIDER_AICPU=1

python -m pip install -v -e . --no-build-isolation
```

`setup.py` reads `BUILD_KV_CLIENT_PROVIDER_AICPU=1` and passes
`-DBUILD_KV_CLIENT_PROVIDER_AICPU=ON` to CMake.

At present this produces an **AICPU plus FAKE** build. FAKE remains ON because
it is CMake's default and `setup.py` does not yet read a
`BUILD_KV_CLIENT_PROVIDER_FAKE` environment variable.

## Build UCM ASU with AICPU only

Use direct CMake when FAKE must be explicitly disabled. This installs the UCM
CMake component to the stated local prefix; it is not a Python editable install.

```bash
cmake -S . -B build-ucm-asu-aicpu \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UCM_ASU=ON \
  -DBUILD_UNIT_TESTS=OFF \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_KV_CLIENT_PROVIDER_FAKE=OFF \
  -DBUILD_KV_CLIENT_PROVIDER_AICPU=ON \
  -DBUILD_KV_CLIENT_PROVIDER_AIV=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build-ucm-asu-aicpu/install"

cmake --build build-ucm-asu-aicpu -j"$(nproc)"
cmake --install build-ucm-asu-aicpu --component ucm
```

## Notes on configuration and cache

- `RUNTIME_ENVIRONMENT` is a CMake variable. It is needed when invoking CMake
  directly. `PLATFORM` is an environment variable used only by `setup.py`.
- `BUILD_UNIT_TESTS` defaults to OFF. The pip route does not pass it as ON and
  does not run `ctest`.
- CMake cache values persist in an existing build directory. Do not reuse a
  build directory configured with different provider options unless the desired
  options are explicitly passed again or the build directory is recreated.
- `setup.py` currently exposes environment controls for AICPU and AIV, but not
  for FAKE.

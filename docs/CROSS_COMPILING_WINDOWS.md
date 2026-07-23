# Cross-compiling Windows builds on Linux

ReXGlue can be built for Windows on a Linux host with `clang-cl`, LLVM's
Windows tools, and an MSVC-compatible sysroot produced by
[xwin](https://github.com/Jake-Shadle/xwin).

## Prerequisites

- Clang 18 or newer with `clang-cl`
- `lld-link`, `llvm-lib`, `llvm-rc`, and `llvm-mt`
- Ninja and CMake 3.25 or newer
- xwin

Create an x86-64 Windows sysroot. Accepting the Microsoft license is required:

```sh
xwin --accept-license --arch x86_64 splat \
  --use-winsysroot-style \
  --preserve-ms-arch-notation \
  --output "$HOME/.xwin"
```

Initialize the vendored dependencies and configure ReXGlue with the supplied
toolchain file:

```sh
git submodule update --init --recursive
XWIN_ROOT="$HOME/.xwin" cmake -S . -B out/build/win-amd64-release -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/xwin-clang-cl.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DREXGLUE_REGISTER_PACKAGE=OFF
cmake --build out/build/win-amd64-release
```

The normal Windows backend defaults still apply. To build a Vulkan-only SDK,
add `-DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON` when configuring.
Explicit-prefix and CI installs should keep `REXGLUE_REGISTER_PACKAGE` disabled
to avoid modifying the host user's CMake package registry.

When consuming another cross-compiled package outside the xwin sysroot, pass
its install prefix through `CMAKE_FIND_ROOT_PATH`, for example:

```sh
cmake -S app -B app-build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/xwin-clang-cl.cmake \
  -DCMAKE_FIND_ROOT_PATH="$HOME/.xwin;/path/to/rexglue-sdk"
```

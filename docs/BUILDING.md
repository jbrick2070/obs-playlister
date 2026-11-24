# Playlister: building from source

How to build [Playlister](../README.md) and run its tests. Back to the [README](../README.md#building).

| System | Needs |
| :--- | :--- |
| Windows 11 x64 | Visual Studio 2022 (or newer) C++ build tools, Windows SDK 10.0.26100, CMake 3.28 or newer |
| macOS 13 or later | Xcode 16 or newer, CMake 3.28 or newer (builds a universal arm64 and x86_64 Xcode project) |
| Linux | The installed OBS and its development files (`obs-studio`), `cmake`, `ninja`, `simde` and libcurl's development files (`libcurl4-openssl-dev` on Ubuntu) |

On Windows and macOS the first configure downloads the OBS 33.0.0 source and the matching obs-deps into `.deps/`
and builds libobs once. From the repository root:

```
# Windows
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure --no-tests=error
cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo

# macOS
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure --no-tests=error

# Linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error
```

Manual OBS checks are listed in [TESTING.md](../TESTING.md).

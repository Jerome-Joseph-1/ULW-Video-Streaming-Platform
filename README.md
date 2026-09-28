# ULW

A C++23 video platform for Linux: resumable uploads streamed straight to object storage, an
FFmpeg HLS transcode worker, and a realtime plane for chat, calls and live streams.

## Build

Needs GCC 14 or Clang 19 (with libstdc++ 14), CMake 3.28+, Ninja, and the development
packages for OpenSSL 3, libcurl, libpq and liburing 2.5+.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Other presets: `debug`, `asan`, `tsan`, `ci` (warnings are errors), `release` (LTO), `fuzz`
(clang). `ctest --preset unit` runs the unit tests under ASan/UBSan. Clang 18 cannot be
used: libstdc++ only provides `std::expected` when `__cpp_concepts >= 202002`, which Clang
first reports in 19.

## Checks

```sh
tools/check-boundaries.sh      # layering rules
tools/check-format.sh          # clang-format, --fix to apply
tools/run-clang-tidy.sh build/dev [files...]
```

## Layout

- `core/` domain model and ports; no OS or vendor includes
- `apps/` one directory per binary; `main.cpp` is the only place concrete adapters are named

Design decisions are recorded in [`docs/adr/`](docs/adr/).

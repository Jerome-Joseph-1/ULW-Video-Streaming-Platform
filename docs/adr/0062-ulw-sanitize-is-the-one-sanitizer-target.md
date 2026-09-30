# 0062. `ulw_sanitize` is the one sanitizer interface target

Status: Accepted
Date: 2026-09-29

## Context

Address, undefined-behaviour and thread sanitizers need matching compile and link flags on every
target, and address and thread cannot share a process. The source documents named separate
`ulw_asan` and `ulw_tsan` targets. A target that forgot to link the right one is built
uninstrumented in a sanitizer build, and CMake files need conditionals to link something that
may not exist.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| `ulw_asan` and `ulw_tsan` targets | Each is obvious on its own | Rejected: two names to link everywhere, and an absent target in a normal build |
| Global `CMAKE_CXX_FLAGS` from the preset | Nothing to link | Rejected: applies to third-party code that must be built plain, and to targets that are not ours |
| One interface target, always defined, empty unless `ULW_SANITIZE` is set | Every library and executable links it the same way, the flags live in one file | Accepted |

## Decision

- `cmake/Sanitizers.cmake` defines `ulw_sanitize` as an `INTERFACE` library unconditionally.
- `ulw_add_library` and `ulw_add_executable` in the root file link `ulw_warnings` and
  `ulw_sanitize` `PRIVATE` on every target they create, so a sanitizer build instruments
  everything the project builds and nothing it fetches.
- When `ULW_SANITIZE` is set, the target adds `-fsanitize=${ULW_SANITIZE}
  -fno-omit-frame-pointer -fno-optimize-sibling-calls -g` as compile options and
  `-fsanitize=${ULW_SANITIZE}` as link options. Frame pointers and no sibling-call
  optimisation keep the stack traces in reports complete.
- `ULW_SANITIZE` is a cache string with the values `address,undefined`, `thread` and empty.
  A value containing both `address` and `thread` is a fatal configure error.
- A value containing `undefined` also adds `-fno-sanitize-recover=undefined`: UBSan otherwise
  prints and carries on, and a finding has to fail the test.

## Consequences

- A normal build links an empty interface target: no conditionals, no cost.
- A sanitizer is chosen by the preset (`asan`, `tsan`, `fuzz`), not by editing CMake files.
- Fuzz targets get sanitizers the same way, through the `fuzz` preset's `ULW_SANITIZE`.
- Diverges from the brief in one detail only: the brief lists the flags; the code adds
  `-fno-sanitize-recover=undefined` and the address-with-thread guard.

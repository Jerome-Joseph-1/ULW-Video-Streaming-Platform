# 0061. CMake 3.28, presets version 6, and a `ci` test preset

Status: Accepted
Date: 2026-09-29

## Context

The root file began with `cmake_minimum_required(VERSION 4.3)`, which the brief calls wrong for
portability: a checkout fails on any machine with an older CMake, for no reason the code has.
The build needs Ninja, `CMakePresets.json` with test and workflow presets, and nothing newer.
Contributors and CI should run the same commands, and the sanitizer and release builds should
not be invented in shell scripts.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep the higher minimum in the root file | Nothing to change | Rejected: it excludes machines with an older CMake (the development machines have 3.28.3), and nothing the build uses needs it |
| Presets version 3 or 4 | Older tools read it | Rejected: test presets with `noTestsAction` and workflow presets need version 6 |
| Build variants as shell scripts or a Makefile | Familiar | Rejected: two sources of truth for flags, and CI drifts from what a developer runs |
| CMake 3.28 minimum, presets version 6, one preset per configuration, all consumers going through them | One definition, tool version is the oldest that does the job | Accepted |

## Decision

- `cmake_minimum_required(VERSION 3.28)` in the root `CMakeLists.txt`, and
  `cmakeMinimumRequired` 3.28.0 in `CMakePresets.json`, which is `version` 6.
- Configure presets: `base` (hidden: Ninja, `build/${presetName}`, compile commands, tests on),
  `dev` (RelWithDebInfo), `debug`, `asan` (Debug, `address,undefined`), `tsan` (Debug,
  `thread`), `ci` (inherits `dev`, adds `ULW_WERROR=ON`), `release` (Release, `ULW_LTO=ON`) and
  `fuzz` (clang-19, `ULW_FUZZ`, `address,undefined`).
- Build presets: one per configure preset except `base`, seven in all, including `tsan` and
  `release`.
- Test presets: `dev` and `ci` (everything), `unit` (on the `asan` build, label `unit`),
  `tsan-unit` (on `tsan`), `conformance` and `integration` (on `ci`, by label). Every test preset
  sets `noTestsAction: error`, so a label that selects nothing fails instead of passing, and a
  600 s timeout.
- One workflow preset, `ci`: configure `ci`, build `ci`, test `ci`. The `ci` test preset is bound
  to the `ci` configure preset because the build that is tested must be the `-Werror` one.
- `ULW_LTO` makes `check_ipo_supported` a hard error when unsupported; a release build that
  silently drops LTO would ship a different binary than it claims.

## Consequences

- The checkout builds on CMake 3.28.3, and a newer tool needs no change.
- `cmake --workflow --preset ci` is the whole CI build on a developer machine.
- `unit` runs on ASan and `tsan-unit` on TSan; nothing selects both, as ADR-0062 forbids.
- Matches the brief: presets and minimum are as decided. Not in the brief: `noTestsAction`, the
  600 s timeout and the hard error for unsupported LTO.

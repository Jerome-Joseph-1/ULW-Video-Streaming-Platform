# 0022. Clang 19 is the minimum Clang

Status: Accepted
Date: 2026-09-28

## Context

Code returns `std::expected` throughout (ADR-0008). libstdc++ 13 and 14 declare `std::expected`
only when `__cpp_concepts >= 202002L`. Clang 18 reports `201907L`, so with Clang 18 and
libstdc++ the `<expected>` header declares nothing: code that uses it does not compile, and
clang-tidy 18 cannot parse it. Clang 19 reports `202002L`. GCC 14 is the other compiler; Clang
builds the sanitizer and fuzz presets, and clang-tidy parses every file.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Clang 18 with libc++ | Clang 18 is Ubuntu 24.04's default, and libc++ 18 ships `std::expected` | Rejected: a second standard library whose behaviour diverges from the GCC build, so a bug could exist in one build and not the other |
| Clang 18 with `__cpp_concepts` defined by hand | One flag; no toolchain change | Rejected: it lies to the library about a language feature level the compiler does not claim |
| GCC only | One toolchain | Rejected: loses libFuzzer and clang-tidy |
| Clang 19 as the minimum | Reports the value libstdc++ checks; packaged for Ubuntu 24.04 | Accepted |

## Decision

Clang 19 is the minimum Clang, and clang-tidy 19 likewise. clang-format stays at 18 for the
formatting gate (`tools/check-format.sh`). GCC 14 is unchanged.

## Consequences

- Distributions that ship only Clang 18 need GCC 14 or a Clang 19 package to build.
- CI installs two LLVM versions: 19 for building and clang-tidy, 18 for clang-format.
- clang-format is pinned separately because its output changes between versions. Moving it to
  19 is one commit that reformats the tree, done when the gate moves.
- Reopen if libstdc++ changes the guard, or when the formatting gate moves to a newer
  clang-format.

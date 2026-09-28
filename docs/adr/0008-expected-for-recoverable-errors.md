# 0008. std::expected for recoverable errors, exceptions for bugs

Status: Accepted
Date: 2026-09-28

## Context

Each gateway process runs a single-threaded reactor. An exception that escapes a callback unwinds
through the loop with every connection in whatever state it was in. Most failures in the domain
and in the ports are ordinary outcomes a caller must handle: an upload offset mismatch, a
throttled store, a lost job lease. The code is C++23 built with GCC 14 and Clang 19 (ADR-0022),
both of which ship `std::expected`.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Exceptions for all errors | Idiomatic; clean happy path; constructors can fail | Rejected: failure paths are invisible in signatures, and nothing that throws can be called from a `noexcept` callback without a `try` at every call |
| Error codes with out-parameters | Familiar from C APIs; no overhead | Rejected: nothing ties the value to the error, and both can be ignored |
| `tl::expected` or Boost.Outcome | Available before C++23 | Rejected: the standard type exists on both compilers; a dependency for nothing |
| `std::expected<T, E>` for actionable errors, exceptions only for bugs | The error is part of the signature and `[[nodiscard]]` makes ignoring it a warning | Accepted |

## Decision

- Domain and port functions return `std::expected<T, E>` for every error a caller can act on.
  `E` is a closed `enum class` per layer (`core::DomainError`, `core::ports::StorageError`), and
  every fallible function is `[[nodiscard]]`.
- Exceptions mean a programmer bug: a violated precondition or an impossible state. They are
  caught once, at the top of the loop, and nowhere else. They are never used for control flow.
- Reactor callbacks are `noexcept`. A throw inside one terminates the process instead of
  unwinding through the loop.

## Consequences

- Every call site handles or propagates the error by hand; `and_then` and `transform` shorten
  chains but propagation stays more verbose than with exceptions.
- Adding a case to an error enum forces every `switch` over it to be revisited, since those
  switches have no `default:`.
- An enum carries no context. Log lines at the point of failure have to say what failed.
- Monitor exceptions caught at the top of the loop; the expected count is zero.
- Reopen if callers routinely need structured context (causes, backend messages) that an enum
  cannot carry.

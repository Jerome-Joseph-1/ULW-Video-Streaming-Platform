# 0034. Layered configuration from a TOML subset, the environment and flags

Status: Accepted
Date: 2026-09-29

## Context

Until now both binaries read the environment only, because arguments are visible to every
local user through `/proc` and the database URL carries a password. Brief 8.14 asks for
defaults < TOML file < environment < command line, everything validated with exit code 2 on a
bad value, and the effective configuration logged with secrets redacted. The deploy manifests
already set the environment variable names, which must keep working.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| toml++ 3.4, pinned in `cmake/Dependencies.cmake` | Complete TOML 1.0 | Rejected: 17,000 header lines to read a file of about twenty scalars, exceptions by default, and every feature we do not use is parsing surface |
| A subset parser of our own, with tests and a fuzz target | Small enough to review whole; refuses what it does not implement | Accepted |
| Keep environment only | Nothing new | Rejected: the brief asks for files and flags |

## Decision

- Every setting is named by its environment variable, the canonical name; it has a file key
  (`listen.port`) and a flag made from that key (`--listen-port`). Some are environment-only
  (`HOSTNAME`, `PATH` for the worker). `--config PATH` or `ULW_CONFIG` names the file.
  `--check-config` validates, logs the result and exits; `--version` prints version and commit.
- The layers produce a lookup by variable name, which the existing loaders read unchanged; the
  defaults stay beside the code that reads each value. An empty value counts as unset in every
  layer.
- Secrets (`ULW_DATABASE_URL`) are refused on the command line, and in a file that any bit of
  group or other permission lets someone else see (0400 or 0600 only). Object store keys stay
  environment-only, as their provider reads them. Unknown file keys and flags are errors.
- `ops::toml` accepts comments, `[tables]` and dotted bare keys, one-line basic and literal
  strings with every escape, decimal integers with `_` separators, and booleans, in UTF-8.
  Arrays, inline tables, floats, dates, multi-line strings, quoted keys and arrays of tables
  are refused with their line number, as are duplicate keys and tables and a value reused as a
  table. A fuzz target checks that whatever it accepts survives a round trip; three minutes of
  libFuzzer (5.2 million inputs) found nothing.
- Configuration errors exit 2; the systemd units set `RestartPreventExitStatus=2`. Invariants
  checked, gateway: port 1..65535; reactor, transport and storage choices; TLS files only with
  TLS and then both; a filesystem read URL only for the filesystem store; `JWKS_URL` https and
  exclusive with a development key set; chunk size between S3's 5 MiB and 5 GiB part limits and
  such that a 50 GiB upload needs at most 10,000 parts (so at least 5.12 MiB);
  `max_upload_slots <= max_connections`; `max_uploads_per_user <= max_upload_slots`; the
  connection string parses (the reason is never quoted, since libpq's quotes the password); the
  R2 account id or MinIO endpoint makes a store profile and both store keys are set; the
  development key set reads and holds a usable key; the TLS certificate and key load and
  match; and, once the descriptor limit is raised, `max_connections <= (RLIMIT_NOFILE - 64) /
  2`. Worker: node id an RFC 1123 label, absolute paths, 1..64 ffmpeg threads, storage choice,
  location and keys, and the connection string. Whatever can be checked without starting
  anything is checked here, so `--check-config` refuses it and a unit that could never start
  exits 2 once instead of restarting every two seconds.
- The new gateway settings are `ULW_CHUNK_SIZE`, `ULW_MAX_CONNECTIONS`, `ULW_MAX_UPLOAD_SLOTS`,
  `ULW_MAX_UPLOADS_PER_USER` and, for both binaries, `ULW_LOG_LEVEL`; defaults are ADR-0009's
  and ADR-0027's.

## Consequences

- A file that needs a TOML feature outside the subset is refused at startup, never half-read.
- Deployments that only set environment variables behave as before.
- Adding a setting means a schema entry, a loader line and a line in the effective-config log.

# 0089. The sandbox runs only the ffmpeg and ffprobe it was built with

Status: Accepted
Date: 2026-10-03

## Context

`ulw_sandbox` (ADR-0025, 0048) is the helper every ffmpeg and ffprobe child starts through. It
took the program to run from its own argv, `-- PROGRAM ARGS...`, and `execv`'d that path once
`check_program` had vetted it (absolute, normal form, an executable regular file). The worker
and the live packager filled PROGRAM from `ULW_FFMPEG` and `ULW_FFPROBE` (default `ffmpeg` and
`ffprobe`), which `process.cpp` resolved against the children's `PATH`.

CodeQL's `cpp/uncontrolled-process-operation` (ADR-0072) flags that `execv`: the file executed
comes from the helper's command line. `check_program` narrows what may run, but not to
ffmpeg: whoever controls the helper's arguments, or the worker's environment or configuration
file, chooses a file to execute, and every executable on the image is one. The only programs the
platform ever runs through the helper are ffmpeg and ffprobe, from the image's own packages
(ADR-0074).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Dismiss the alert: `check_program` already vets the path | No change to deployments | Rejected: the vetting bounds what kind of file runs, not which; any executable regular file on the image passes |
| Keep `ULW_FFMPEG`/`ULW_FFPROBE`, and have the helper accept a path only from an allowlist read at run time | Operators keep choosing a binary without a rebuild | Rejected: the allowlist is then input too, and the file executed still depends on what the process was given |
| The helper takes a name (`ffmpeg`, `ffprobe`) and looks it up in a table compiled into it from CMake cache variables, `ULW_SANDBOX_FFMPEG` and `ULW_SANDBOX_FFPROBE` (default `/usr/bin/ffmpeg`, `/usr/bin/ffprobe`) | The file executed is a constant chosen by a lookup; nothing at run time can name another. The images already install Debian's ffmpeg at those paths | Accepted |
| Hard-code `/usr/bin/ffmpeg` in the source | Simplest | Rejected: a host or image with ffmpeg elsewhere could not run it at all; a cache variable keeps that a build-time choice |

## Decision

- `ulw_sandbox ... -- NAME [ARGS...]`: NAME is a program name, never a path. The helper finds it
  in `kPrograms`, the table CMake generates into `sandbox_programs.hpp`
  (`infra/ffmpeg/CMakeLists.txt`, `ulw_sandbox_programs`), and `execv`s the path the table holds,
  with NAME as the program's argv[0] and ARGS after it. The production helper's table holds
  `ffmpeg` and `ffprobe` and nothing else.
- A name with a slash in it is refused (126) even when it is a path the table holds; any other
  name not in the table is not found (127). Both come before any confinement step, as the path
  checks did. `check_program` still vets the table's path, as defence in depth (126, or 127 for a
  file that is not there).
- `ULW_SANDBOX_FFMPEG` and `ULW_SANDBOX_FFPROBE` must be absolute paths in normal form, without
  quotes, backslashes or generator expressions; configure fails otherwise. They are the only way
  to point the platform at another ffmpeg: build the image with them set.
- The adapter's command lines name the programs `ffmpeg` and `ffprobe` (`kFfmpeg`, `kFfprobe` in
  `infra/ffmpeg/src/command.hpp`). `TranscoderConfig`, `RecordingRemuxConfig` and
  `LiveRemuxConfig` lose their program paths, and `process.cpp` its `PATH` lookup.
  `PATH` is still passed to the children, and used for nothing else.
- `ULW_FFMPEG` and `ULW_FFPROBE` are retired. The worker and the live packager refuse a
  non-empty value at startup (exit 2) rather than ignore it, since an operator who set one meant
  some other ffmpeg and would not get it. The worker keeps the settings, and their file keys and
  flags, only to refuse them.
- Tests run through `ulw_sandbox_test_helper`, built in `tests/CMakeLists.txt` from the same
  source with a table of its own: ffmpeg and ffprobe as in production, ordinary programs that
  stand in for ffmpeg (`sh`, `bash`, `cat`, `env`, `sleep`, `true`, `python3`), the syscall
  probe, and one path wrong in each way `check_program` refuses. It has no install rule, and the
  Dockerfile copies only `ulw_sandbox`.

## Consequences

- The `execv` in `sandbox_main.cpp` takes its file from a constant table selected by a lookup;
  only the program's arguments still come from the helper's argv, which is inherent.
- An operator can no longer run another ffmpeg by setting a variable. A different ffmpeg means
  a different image, which the trixie workflow and the image builds already test as a whole
  (ADR-0074). A host-built worker (`deploy/systemd`) uses the distribution's `/usr/bin/ffmpeg`
  unless built with the variables.
- A deployment that still sets `ULW_FFMPEG` or `ULW_FFPROBE` stops at startup with a message
  naming the build variables. None of the manifests in this repository sets either.
- The production helper's table is checked by `ProductionPrograms.AreExactlyFfmpegAndFfprobeAtTheirBuiltInPaths`,
  and the production helper refusing anything else by
  `SandboxTest.TheProductionHelperRunsOnlyFfmpegAndFfprobe` (`tests/unit/ffmpeg`).

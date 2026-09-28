# 0025. FFmpeg runs as a sandboxed subprocess

Status: Accepted
Date: 2026-09-28

## Context

transcode_worker holds a job lease (ADR-0006) while it turns an untrusted upload into a 3-rung
HLS fMP4 ladder. Decoders parse hostile input, and libav* has a long history of memory-safety
bugs. A decoder crash must not take down the process that holds the lease, and a malicious file
must not reach the network or the rest of the filesystem.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Link libav* and transcode in-process | No process start; direct access to frames; fine-grained progress | Rejected: a decoder crash or memory corruption kills the worker holding the lease, with no OS boundary around hostile input |
| A Kubernetes Job per transcode | The strongest isolation, with limits per job | Rejected: pod start-up on every video, and the worker would need permission to create pods |
| `ffmpeg` and `ffprobe` via `posix_spawn`, sandboxed | A crash kills a child, not the worker; namespaces and resource limits apply per child | Accepted |

## Decision

- The worker runs `ffprobe` and `ffmpeg` as subprocesses started with `posix_spawn`. It does not
  link libav*.
- Each child is sandboxed: `unshare -n` (an empty network namespace), a pid namespace of its own
  with its own `/proc`, a read-only root with one writable scratch directory, `RLIMIT_AS` and
  `RLIMIT_CPU`, and a wall-clock deadline after which the worker kills it. When the child's
  pid 1 exits, the kernel kills whatever the child started, so nothing outlives it.
- The children open an upload only with a closed list of container demuxers
  (`-format_whitelist`); manifest formats such as DASH would read other local files.

## Consequences

- Progress is parsed from `-progress pipe:1`, the key=value lines ffmpeg writes to stdout.
- An exit code above 128 means the child was killed by a signal (128 + signal number): a crash, the
  CPU limit or our deadline. It is classified apart from ffmpeg's own non-zero exits, which mean
  it rejected the input. The CPU limit is recognised by the child's CPU time reaching it, not by
  the signal: ffmpeg catches SIGXCPU and dies of the SIGKILL at the hard limit. Hitting `RLIMIT_AS` shows up as an allocation failure inside ffmpeg and
  may be an ordinary error exit rather than a signal.
- The worker depends on the ffmpeg CLI's arguments and progress format, so the worker image pins
  the ffmpeg version.
- The namespaces need `CAP_SYS_ADMIN` or unprivileged user namespaces in the worker's pod, and
  container runtime seccomp defaults can refuse it. Monitor sandbox setup failures; the worker
  fails the job rather than running ffmpeg without the sandbox.
- Reopen if the pipeline needs frame-level access in-process that the CLI cannot express.

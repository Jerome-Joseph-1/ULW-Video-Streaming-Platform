# 0078. Large blocks go back to the kernel

Status: Accepted
Date: 2026-10-02

## Context

The soaks (ADR-0042, ADR-0069) hold each service's resident memory to a per-unit bound after a
15 minute warm-up. Every chat and gateway run since 2026-09-30 failed it while doing nothing
wrong: no transport errors, flat descriptors, and memory that climbs fast at first and then ever
more slowly. On main a81c25b chat grew about 3 MB/h over minutes 15–90, 0.2–0.7 MB/h over
minutes 90–160 and 0.1–0.2 MB/h over minutes 160–240, against a bound near 10 KB/h; the gateway
went 180, 82 and 40 KB/h (`docs/operations/soak.md`).

The live heap does not grow with it. Sampled with `malloc_info`, a chat node's in-use bytes fell
from 13.3 to about 10 MB after the warm-up while the heap glibc held rose from 14.1 to 15.3 MB,
with 4–5 MB free inside it in about 3000 holes. Each delivered message passes through several
transient buffers of 32 to 87 KiB: the message text, its base64, the WebSocket frame and its
encoding. Below glibc's mmap threshold, which starts at 128 KiB and rises to 32 MiB as large
blocks are freed, those come from the heap, and a burst of them leaves holes that later small
allocations pin, so the heap's top creeps up and stays there.

Three chat clusters run side by side for an hour (minutes 15–60, mean RSS slope of three
nodes):

| Build | Slope |
|---|---|
| main | +858 KB/h |
| main with large blocks mapped (`glibc.malloc.mmap_threshold=16384`) | +346 KB/h |
| that, plus send chunks returned and message buffers reused | +211 KB/h |

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Set the threshold in the deployment (`GLIBC_TUNABLES`) | No code | Rejected: every runner of the binary has to remember it, and a soak or a pod without it measures something else |
| Link jemalloc or mimalloc | Better at long-lived fragmentation | Rejected for now: a new pinned dependency and a larger change than the measured cause needs |
| Call `malloc_trim` on a timer | One line | Rejected: it gave back 1.2 of about 4 MB free in a run, since the holes are mostly smaller than a page apart |
| Loosen the soak bound or lengthen its warm-up | The climb slows | Rejected: the bound is the acceptance criterion; the memory has to stay flat |
| Fix the threshold in code at 16 KiB, at startup | Holds wherever the binary runs | Accepted |

## Decision

- `ops::return_large_blocks()` sets `M_MMAP_THRESHOLD` to 16 KiB. chat_server, gateway_server
  and transcode_worker call it at startup, after `ops::disable_core_dumps` and before anything
  is allocated in earnest. Setting the threshold also stops glibc raising it, so freed large
  blocks no longer drag it up.
- glibc still serves a large request from a free hole it already has; only when the heap would
  grow does the block get a mapping of its own, which `free` returns. The holes are reused and
  the heap's top stops creeping.
- A sanitizer's allocator refuses `mallopt`; the service logs that and runs on.

## Consequences

- An allocation of 16 KiB or more that cannot be served from a hole costs an `mmap` and a
  `munmap`. Chat's per-message buffers are reused (send chunks and message texts, the commits
  beside this one), so few such allocations remain on the delivery path.
- The setting lowers the climb but does not by itself bring chat within the bound: about
  0.2 MB/h remains after an hour, which is being traced with heaptrack.
- `ReturnLargeBlocks.BlocksOfThirtyTwoKibPastTheHeapsFreeSpaceAreMappedAndReturned` fails
  without the setting.

# 0081. chat_server allocates with jemalloc

Status: Accepted, gateway_server's glibc malloc amended by 0094 (one arena, fixed mmap threshold)
Date: 2026-10-02

## Context

The soaks (ADR-0042, ADR-0069) hold each service's resident memory to a per-unit bound after a
15 minute warm-up. Every chat and gateway run since 2026-09-30 failed it while doing nothing
wrong: no transport errors, flat descriptors, and memory that climbs fast at first and then ever
more slowly. In a four-hour run of both soaks on main a81c25b (2026-10-01, on a development host) chat grew
about 3 MB/h over minutes 15–90, 0.2–0.7 MB/h over minutes 90–160 and 0.1–0.2 MB/h over
minutes 160–240, against a bound near 10 KB/h; the gateway went 180, 82 and 40 KB/h.

The live heap does not grow with it. Sampled with `malloc_info`, a chat node's in-use bytes fell
from 13.3 to about 10 MB after the warm-up while the heap glibc held rose from 14.1 to 15.3 MB,
with 4–5 MB free inside it in about 3000 holes. Each delivered message passes through transient
buffers of 32 to 87 KiB (the message text, its base64, the WebSocket frame); a burst of them
leaves holes that later small allocations pin, so the heap's top creeps up and stays there.

Two commits beside this one take what they can out of the delivery path: send chunks are
mappings of their own, kept for the recent peak and unmapped after it (`net/src/send_queue.cpp`),
and chat grows its message texts and frames once into buffers it reuses. They lowered the climb
but did not end it. The allocator was then compared directly, on the self-hosted soak host, all
from one build of that branch, side by side, two hours each, judged after the warm-up (the
soak-experiment workflow's runs 37023997323 and 37039160488):

| Allocator | chat-1 | chat-2 | chat-3 | Verdict |
|---|---|---|---|---|
| glibc 2.39 | +100 KB/h | +162 KB/h | +223 KB/h | fail |
| glibc, mmap threshold fixed at 16 KiB (`mallopt`) | +211 KB/h, mean of three, one hour | | | fail |
| mimalloc 2.1.2 | +92 KB/h | +219 KB/h | +423 KB/h | fail |
| jemalloc 5.3.0 | −1049 KB/h | −1055 KB/h | −804 KB/h | pass |
| jemalloc 5.3.0, again | −1280 KB/h | −822 KB/h | −902 KB/h | pass |

The gateway, measured the same way in the second run, grew about as fast on either allocator:
+112 KB/h on glibc and +146 KB/h on jemalloc (95% upper ends +136 and +360), against a bound near 30 KB/h at the
soak's load. Its growth is not the allocator's, and is traced separately.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Fix glibc's mmap threshold in code (`mallopt`) | No new dependency | Rejected: it lowered the climb and did not end it (table) |
| `GLIBC_TUNABLES` in the deployment | No code | Rejected: as weak as the above, and every runner of the binary must remember it |
| `malloc_trim` on a timer | One line | Rejected: it gave back 1.2 of about 4 MB free in a run, since the holes are mostly smaller than a page apart |
| mimalloc | Small, fast, returns memory | Rejected: measured no better than glibc here (table) |
| jemalloc | Purges dirty pages on a decay timer and keeps size classes apart, so transient buffers do not pin long-lived ones | Accepted for chat_server: the only allocator measured flat, twice |
| jemalloc for gateway_server too | One allocator for both | Rejected for now: no better than glibc there (above) |
| Loosen the soak bound or lengthen its warm-up | The climb slows | Rejected: the bound is the acceptance criterion; the memory has to stay flat |

## Decision

- chat_server links jemalloc (`ulw::allocator`, `ULW_JEMALLOC`, on by default). The version is
  the one measured: Ubuntu's 5.3.0-2build1 (`libjemalloc-dev` in CI). chat_server has no image
  yet; when it gets one, its build and runtime stages pin `libjemalloc-dev` and `libjemalloc2`
  to that version from the snapshot like the other packages (ADR-0030). The image that exists
  builds no binary that links it, so it configures with `-DULW_JEMALLOC=OFF`. jemalloc's
  defaults are kept; `MALLOC_CONF` tunes it where that is ever needed.
- gateway_server, transcode_worker, the tools and the tests keep glibc's malloc: the gateway
  grew alike on both, the worker is flat on glibc in every soak, and the tests should not depend
  on the allocator.
- Never under a sanitizer, whose own allocator jemalloc would replace: the `asan` and `tsan`
  presets build the services with glibc's malloc.
- chat_server and gateway_server log the allocator they got at startup (`"allocator"`:
  `"jemalloc"` or `"default"`, glibc's or a sanitizer's, with
  jemalloc's version through `ops::jemalloc_version()`, which finds its `mallctl` at run time),
  and `ChatClusterTest.EveryNodeAllocatesWithTheAllocatorTheBuildLinked` fails when the build
  says jemalloc and a node runs without it.

## Consequences

- One more library for chat_server, which its future image's vulnerability gate (ADR-0072)
  covers like the rest.
- glibc's malloc tunables (`GLIBC_TUNABLES=glibc.malloc.*`, ADR-0069's diagnostics) no longer
  reach chat_server; jemalloc's `MALLOC_CONF` does, and its statistics
  (`MALLOC_CONF=stats_print:true`) and heap profiles (`prof:true`, read with `jeprof`) replace
  `malloc_info` for the same questions.
- The gateway's soak still fails its memory bound until its own growth is found and fixed.
- A soak of a sanitizer build measures glibc's allocator, not the one the services ship with;
  the soaks build the `ci` preset, which links jemalloc.

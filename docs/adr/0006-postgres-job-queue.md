# 0006. The job queue is a Postgres table

Status: Accepted
Date: 2026-09-28

## Context

When an upload commits, its video moves to processing and a transcode job must exist for it.
The state change and the job must never disagree: a video stuck in processing with no job, or a
job for a video whose state change rolled back, are both bugs that only show up later. Postgres
already holds video state. The cluster is two VPS nodes without autoscaling; every stateful
service added is one more thing to run, back up and upgrade.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| A message broker (RabbitMQ, NATS, Redis streams) | Push delivery; retries and dead-lettering built in | Rejected for v1: a second stateful service on two nodes, and enqueueing becomes a dual write beside the state change, which can half-succeed |
| Postgres table, polling only | One store; transactional enqueue | Rejected: pickup latency equals the poll interval, and short intervals cost queries |
| Postgres table claimed with `FOR UPDATE SKIP LOCKED`, woken by LISTEN/NOTIFY, polled as a backstop | Transactional enqueue; workers never wait on each other's claims; fast pickup when notifications arrive | Accepted |

## Decision

- Jobs are rows in `jobs`. A job is inserted in the same transaction as the video state change
  that calls for it.
- A worker claims with `SELECT ... FOR UPDATE SKIP LOCKED LIMIT 1` and, in the same transaction,
  sets the lease and increments the job's `fence` column.
- Heartbeat and finish are `UPDATE ... WHERE id = $1 AND fence = $2`. Zero rows updated means the
  lease was lost; the worker abandons the job without recording a result.
- Enqueueing sends `NOTIFY`. It is an optimisation only: a notification sent while no worker is
  listening is gone. Every worker also polls about every 5 s, so a lost notification costs at
  most 5 s of latency, never a job.
- No broker in v1. If one is added, it is fed from a transactional outbox table written in the
  same transaction as the state change, never by a dual write from application code.

## Consequences

- A job whose lease expires is claimed again. A fenced-out worker may already have written
  objects, so worker output must be safe to overwrite by the next attempt.
- LISTEN needs a session-level connection per worker; it does not work through a
  transaction-mode pooler.
- Polling costs one query per worker every 5 s, which is negligible at our worker counts.
- Monitor queue depth, the age of the oldest unclaimed job, lease expiries (each one is a worker
  that died or stalled) and fenced-out updates.
- Reopen when jobs need several independent consumers or a volume a table cannot carry; the
  broker then arrives with an outbox.

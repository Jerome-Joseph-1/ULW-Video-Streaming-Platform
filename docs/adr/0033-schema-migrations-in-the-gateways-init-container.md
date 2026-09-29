# 0033. Schema migrations run in the gateway's init container

Status: Accepted
Date: 2026-09-29

## Context

`ulw_migrate` applies the migrations bundled into it under an advisory lock, and exits with an
error rather than wait when another migrator holds the lock. On Askedin's cluster
Woodpecker builds and pushes images and runs `rollout restart`; it never applies manifests.
ArgoCD applies the manifests from git, on its own schedule, and may sync a commit before the
image built from it has been pushed. The migrations must run with the image about to serve,
before it serves.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| A Kubernetes Job, created by the pipeline | One run per deploy, visible as its own object | Rejected: creating the Job from Woodpecker is applying a manifest, which the deploy flow forbids |
| A Job as an ArgoCD PreSync hook | ArgoCD's own mechanism for exactly this | Rejected: it runs when ArgoCD syncs git, not when Woodpecker pushes the image, so it can migrate with the previous build's migrations, and never runs on a plain `rollout restart` |
| Migrate at gateway start-up, in-process | No extra container | Rejected: the server would need DDL rights for its whole life and would mix migration failures into serving start-up |
| An init container in the video-gateway pod running `ulw_migrate` from the same image | Runs on every rollout, restart included, with exactly the build that is about to serve | Accepted |

## Decision

- The video-gateway Deployment has an init container `migrate` with the gateway image and
  `ulw_migrate` as its command, and the gateway's `ULW_DATABASE_URL`.
- The rolling update brings up one new pod at a time (`maxSurge: 1`, `maxUnavailable: 0`), so
  normally one migrator runs. When pods start together (bootstrap, or a node restart) the loser
  of the advisory lock exits non-zero and the kubelet restarts it a few seconds later, when it
  finds the schema current.
- Migrations only ever add: a column, a table, an index. The pod still on the old image keeps
  serving against the new schema until it is replaced. A change that removes or renames is split
  into an additive migration and a later removal, a release apart.

## Consequences

- The gateway's database role needs DDL rights. Splitting a migration role from the serving
  role would take a second secret key and a second connection string.
- A failing migration keeps the new pod in `Init:CrashLoopBackOff`; the old pods keep serving,
  so a bad migration stops a rollout rather than an outage. Watch for it after every deploy.
- Both containers name the same mutable branch tag with `imagePullPolicy: Always`, and the
  kubelet pulls them one after the other. A push landing between the two pulls of one pod start
  pairs build B's server with build A's migrations; if B needs a migration of its own, its
  queries against the missing column or table fail until the pod restarts. The window is the
  seconds between the pulls, and Woodpecker's own restart follows every push, which runs both
  from the new image; pinning the containers to a digest would close it, at the price of ArgoCD
  seeing the live image differ from git after every deploy. After a deploy that adds a
  migration, check `kubectl logs <pod> -c migrate` names it, and restart once more if not.
- The worker is not ordered after the migration; Woodpecker restarts it only after the gateway's
  rollout completes (deploy/askedin/woodpecker.yml).
- Reopen if the deploy flow gains a step that may apply manifests, or migrations become slow
  enough to hold up a rollout.

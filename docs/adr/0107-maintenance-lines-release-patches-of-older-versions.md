# 0107. Maintenance lines release patches of older versions from release/vX.Y branches

Status: Accepted
Date: 2026-10-13
Amends: ADR-0099 (a release may be cut from a maintenance line as well as from `main`)

## Context

ADR-0099 releases a commit on `main` only. Production pins a version and moves to a newer one on
its own schedule: Askedin runs v0.2.0 in production and v1.0.0 elsewhere while `main` carries
v1.x work and migrations up to 0020. A fix it needs now (ADR-0106) cannot wait for it to adopt
`main`'s tip, and cutting a patch from `main` would ship every change since, migrations included.

What had to hold: a maintenance release keeps ADR-0099's guarantees (the commit passed `ci`'s
full lane, the images passed publish-images' gates and are attested, a version names one digest
for ever); `:main` keeps meaning main's tip; and only the repository's writers can start one.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Release patches from `main` only | Nothing new | A patch of v0.2 would carry all of v1.x and its migrations | Rejected |
| A long-lived branch per minor version, `release/vX.Y`, cut from `vX.Y.0`, fixes cherry-picked, its pushes run `ci` and publish, release.yml takes the branch | The same gates and attestations as `main`; one branch per line in production | Each line's branch needs `ci` that runs where it can today (its own ci.yml must be the hosted one, ADR-0104) | Accepted |
| Tag a fix commit made off a tag, with no branch | No branch to keep | No push for `ci` to run on, so neither the publish nor the release would have the run they require | Rejected |

## Decision

- A maintenance line is a branch `release/vX.Y`, cut from the tag `vX.Y.0`. Fixes reach it by
  cherry-pick from `main` after they merge there. A fix that needs a migration is not
  cherry-picked: migration numbers form one line (ADR-0031), main's.
- ci.yml runs its full lane on a push to `release/v*`. A line cut from a tag older than ADR-0104
  first gets ADR-0104's hosted ci.yml by cherry-pick, so its runs can finish.
- publish-images.yml (main's definition, started by `ci`'s success) accepts a successful push run
  of `main` or of a branch named `release/vX.Y`, checks the commit is on that branch and that the
  push run for that branch succeeded, and moves `:main` only for `main`. Its manual run takes
  `branch`.
- release.yml takes `branch` (default `main`). For `release/vX.Y` it accepts only versions
  `vX.Y.Z`, checks the commit is on the branch and its push run there succeeded, creates the
  release with `--latest=false`, and starts its generated notes at `vX.Y.(Z-1)` when that tag exists.
- The branch should be protected like `main` (no force push, no deletion), so a released commit
  stays on it.

## Consequences

- v0.2.1 and v1.0.1 ship from `release/v0.2` and `release/v1.0` with ADR-0106's fix.
- Each line runs the full lane on every push, a few hosted hours each, only when a fix lands.
- A line is retired by no longer pushing to it; its tags and images stay.

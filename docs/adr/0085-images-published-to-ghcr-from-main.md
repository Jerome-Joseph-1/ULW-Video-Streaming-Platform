# 0085. Images are published to GHCR from main; stage follows `main`, prod is pinned

Status: Accepted, amended by 0088 (the pin rule checks the rendered production overlay; the operator pulls and configures) and 0099 (version tags beside the SHA tag)
Date: 2026-10-03
Amends: ADR-0031 and ADR-0083 (Woodpecker no longer builds or pushes the images Askedin runs);
ADR-0072's registry exemption (this repository's own builds are now
`ghcr.io/jerome-joseph-1/ulw-...`, and prod may not name them by `main`)

## Context

Askedin ran the four images (video-gateway, video-worker, chat, live-packager) from its own
registry, `git.askedin.com/askedin/askedin-monorepo/...`, which Woodpecker built and pushed under
the branch's name (`deploy/askedin/woodpecker.yml`), and the overlays followed that name with
`imagePullPolicy: Always` (ADR-0031, ADR-0083). The code, its tests and its gates (`ci`, and
the e2e sandbox's hardening check and Trivy image gate, ADR-0072) live in this GitHub
repository, but the images that ran were built by a second pipeline, outside those gates, with
its own registry credentials to keep.

A branch-named tag is mutable: a restart, a reschedule or a node drain can change which build
runs, and two pods started minutes apart can run different ones. That is acceptable for a
stage environment that should track the newest code, not for prod.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Keep Woodpecker building into Askedin's registry | Nothing to change | Rejected: the running build is not one this repository's gates checked, and building images is maintained in two places |
| Publish from GitHub Actions to private GHCR packages | Built where it is tested | Rejected: every Askedin node would need a pull secret for a GitHub account, rotated by hand |
| Publish from GitHub Actions to public GHCR packages, from `main` only, after `ci` succeeds | One build, checked by the same gates as the e2e sandbox, pulled without credentials | Accepted |
| Tag `latest` | Conventional | Rejected: says nothing about which commit it is |
| Tag each build with the full commit SHA, and `main` for the newest | The SHA names the commit; `main` gives stage a tag to follow | Accepted |
| Every environment follows `main` | One overlay shape | Rejected for prod: a moving tag can change prod's build without a deploy |
| Prod pinned to a SHA published from main, preferably `<sha>@sha256:<digest>` | A prod deploy is a reviewed commit; the digest cannot change under it, the SHA keeps it readable | Accepted |
| Push the SHA tag again on every publish of the same commit | Simpler workflow | Rejected: the image's created label differs per build, so the digest would change and a SHA tag would no longer always mean one digest |

## Decision

- `.github/workflows/publish-images.yml` builds the four Dockerfile targets for each push to
  `main` (and on a manual run, for a commit already on `main`), checks them with the release
  binaries' hardening check and Trivy's image gate (ADR-0072), waits until that commit's `ci`
  run on `main` has succeeded, and only then pushes to
  `ghcr.io/jerome-joseph-1/ulw-<service>`. Pull requests never run it.
- The packages are public. Askedin only pulls the images and configures the overlays; it builds
  nothing. `woodpecker.yml` may keep running, but nothing pulls what its image steps push.
- Each image is tagged with the full commit SHA, pushed once: a commit published before keeps
  the digest it was first published under, and a later run reuses and reports it. `main` moves
  for all four images together, only after all four SHA tags exist, and only while the commit
  is still `main`'s tip. Nothing is tagged `latest`.
- Every pushed digest gets a build provenance attestation (`actions/attest-build-provenance`)
  from the run that first pushed it, verifiable with `gh attestation verify`; a later run that
  finds the SHA tag already published attests nothing, since it did not build that digest.
  `:main` tags the pushed manifest itself (`--prefer-index=false`), so it carries the same
  attested digest. The run's summary lists each image as `<sha>@sha256:<digest>`.
- Stage's overlays follow `:main`. Prod's overlays ship with the placeholder `:<sha>` and are
  set, before prod syncs, to a SHA published from main, preferably `<sha>@sha256:<digest>`
  from that run's summary (deploy/askedin/RUNBOOK.md, 4a). `deploy/local/check-image-pins.py`
  fails a file under `overlays/prod/` that names one of these images by any tag other than
  `<sha>` or a 40-hex SHA, unless it carries a digest. The live packager's Job is filled in
  per stream; on prod its tag is such a SHA as well.

## Consequences

- Publishes run one at a time with one waiting; a newer push replaces a waiting run, so an
  intermediate commit of a burst of merges may never be published (`main` still reaches the
  newest). A publish whose `ci` run failed is not retried when `ci` is re-run. Either commit is
  published by a manual run with its SHA.
- Prod changes build only through a commit in Askedin's overlays; a restart never changes it.
  Stage follows `main` and can change on a restart.
- A rollback is a commit of an older `<sha>@sha256:<digest>`; every published SHA stays.
- Making the packages public is the repository owner's to do once, and cannot be undone.
- The supply chain now includes GitHub Actions, GHCR and Sigstore; the workflow's actions are
  pinned by SHA and linted by actionlint and zizmor (ADR-0072).

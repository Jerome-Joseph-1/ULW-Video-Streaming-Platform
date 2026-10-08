# 0099. A release is a version tag on a commit and images already published from main

Status: Accepted
Date: 2026-10-08
Amends: ADR-0085 (images carry a version tag beside the SHA tag and `main`)

## Context

ADR-0085 publishes the four images for each commit on `main`, tagged with the full SHA and
`main`, and has production pin `<sha>@sha256:<digest>` taken from a publish run's summary. The
teams running ULW (Askedin first) want a named, versioned release to pin and to talk about: one
name for a set of four images, the commit they came from, the integration docs as they were then
and the pull requests since the last release. Nothing in the repository makes one today.

A version must keep ADR-0085's guarantees: only a commit on `main` whose `ci` run succeeded, only
images that passed publish-images' hardening and Trivy gates, and a name that never changes what
it means.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Build and push the images again from a `v*` tag push | The usual pattern | Rejected: a second build of the same commit has another digest (the created label), so the release would not be the build `ci`, the gates and the attestation covered; and anyone who can push a tag could start it |
| A manual workflow that names the digests publish-images pushed for a commit by a version tag, and creates the git tag and the GitHub Release | Nothing is built; the release is exactly what was tested and attested | Accepted |
| Semantic versions computed from commit messages | No input to get wrong | Rejected: the number is a promise about compatibility (docs/integration/versioning.md) that a person makes |
| Allow moving a version tag to a new build | Fixes a bad release in place | Rejected: a version, like a SHA tag, names one digest for ever; a fix is the next patch version |
| Production pins the version tag alone | Readable | Rejected, as `main` is in ADR-0085: a registry tag can be moved by anyone with the package's write access; production pins `<sha>@sha256:<digest>`, which the release lists |

## Decision

- `.github/workflows/release.yml`, run by hand from `main` (so only the repository's writers can
  run it), takes `version` (`vMAJOR.MINOR.PATCH`) and `commit` (main's tip when empty).
- It refuses unless the commit is on `main`, the tag `version` does not exist yet, the commit's
  `ci` run for its push to `main` succeeded, and all four images exist as `:<sha>`. It builds
  nothing.
- It tags each image's `:<sha>` manifest as `:<version>` with `docker buildx imagetools create
  --prefer-index=false`, the same digest and therefore the same provenance attestation. A
  version tag already in the registry is accepted only if it names that digest (a re-run after a
  later step failed); otherwise the run stops.
- It creates the annotated git tag `version` on the commit and a GitHub Release whose notes list
  the commit, each image as `:<version>` and as `<sha>@sha256:<digest>`, the `gh attestation
  verify` command for each, links to the integration docs at the tag, and the pull requests since
  the previous release (GitHub's generated notes).
- Each job has only the permissions it uses: the checks `contents: read` and `actions: read`, the
  image tags `packages: write`, the git tag and release `contents: write`.

## Consequences

- A release is cut in a minute, after publish-images has published the commit; a commit
  publish-images skipped is published first by its manual run.
- Production still pins `<sha>@sha256:<digest>`; the version names the same images, and
  `deploy/local/check-image-pins.py --pinned` still refuses a bare `:<version>` in a production
  overlay.
- A bad release is not changed: the next patch version replaces it, and the bad one's release
  notes can say so.
- Version tags are never deleted by any workflow; deleting one is the package owner's to do by
  hand, and makes that version unresolvable for anyone who pinned the tag rather than the digest.

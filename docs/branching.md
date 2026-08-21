# IRIS branching strategy

## Purpose

Defines how the existing branches are intended to be used so collaboration does not blur stability boundaries.

## Branch roles

The repository has exactly three branches — `main`, `silver` and `staging` —
and in practice they are kept at the same commit: work is committed on `main`
and the other two are fast-forwarded to it.  The roles below describe how they
are *intended* to diverge when parallel work makes that useful; they are not a
description of a divergence that exists today.  A `collab` branch was part of
the original plan and was never created.

- `main`
  - promoted stable line
  - should reflect the most trustworthy repository state
  - no direct experimental work

- `silver`
  - primary working branch
  - default landing zone for active engineering work
  - where focused feature, hardening, and infrastructure changes are prepared

- `staging`
  - integration and verification branch
  - used to combine reviewed changes before promotion to `main`
  - good place for broader validation, conflict surfacing, and release-candidate checks

(`collab`, described in earlier revisions of this document, does not exist.)

## Expected flow

Normal path:

1. create a short-lived topic branch from `silver`
2. land focused work back into `silver`
3. promote validated sets from `silver` into `staging`
4. promote verified `staging` snapshots into `main`

Current path, single maintainer:

1. commit focused work directly on `main`
2. fast-forward `silver` and `staging` to it
3. push all three, creating no new branches

The multi-branch flow above is what to fall back on the moment more than one
line of work is in flight.

## Promotion rules

Promote to `staging` only when:

- CI is green
- required local validation is stated
- docs are updated for changed contracts or workflows

Promote to `main` only when:

- changes have already lived in `staging`
- no known critical boot or bootstrap regressions remain
- the resulting state is something you are comfortable treating as stable

## What not to do

- do not treat `main` as a scratch branch — every commit on it should be a
  green tree (build, `make test-unit`, `make check-purity`, the runtime suite)
- do not accumulate long-lived unrelated work directly in `staging`
- do not create new long-lived branches without a reason that outlives one
  change; short-lived topic branches are fine and should be deleted after
  landing

## Release hygiene

For any promotion candidate, record:

- what changed
- what was validated
- what remains risky or manual

That note can live in the PR description, release note, or promotion summary.

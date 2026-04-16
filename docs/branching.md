# IRIS branching strategy

## Purpose

Defines how the existing branches are intended to be used so collaboration does not blur stability boundaries.

## Branch roles

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

- `collab`
  - collaboration branch
  - preserve for cooperative work that should not destabilize `silver`
  - do not repurpose as a second staging branch

## Expected flow

Normal path:

1. create a short-lived topic branch from `silver`
2. land focused work back into `silver`
3. promote validated sets from `silver` into `staging`
4. promote verified `staging` snapshots into `main`

Suggested collaboration path:

1. use `collab` only when multiple contributors need a shared intermediate branch
2. merge or cherry-pick reviewed results back into `silver`
3. continue promotion through `staging` to `main`

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

- do not treat `main` as a scratch branch
- do not accumulate long-lived unrelated work directly in `staging`
- do not use `collab` as a permanent catch-all integration branch
- do not bypass `silver` unless handling an urgent stability fix with clear justification

## Release hygiene

For any promotion candidate, record:

- what changed
- what was validated
- what remains risky or manual

That note can live in the PR description, release note, or promotion summary.

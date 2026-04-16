# Contributing to IRIS

## Scope

IRIS is a low-level x86_64 OS project. Changes must optimize for stability, reviewability, and architectural clarity over speed or visible feature count.

## Ground rules

- Prefer minimal, surgical edits.
- Do not rewrite working subsystems without a demonstrated reason.
- Keep the current boot path compatible unless the change explicitly targets that area.
- If code and docs disagree, fix the docs or the code in the same change.
- Separate unrelated changes. Avoid mixed refactors.

## Required validation

At minimum, before opening a PR:

- `make clean`
- `make`
- `make check`

If the change affects boot, bootstrap, IPC, handle rights, IRQ routing, initrd contents, service protocols, or diagnostics:

- `make smoke`

If the change can affect runtime bring-up and you can validate locally:

- `make run`

If you cannot run one of the expected validations, state that explicitly in the PR.

## Change shape

Good PR shape:

- one focused problem
- one reviewable fix
- one clear validation story

Avoid PRs that combine:

- CI + unrelated refactor
- protocol changes + cosmetic edits
- boot-path changes + large doc rewrites unless tightly coupled

## Documentation expectations

Update documentation when changing:

- boot/bootstrap sequencing
- service contracts
- syscall semantics
- rights/capability invariants
- branching or contributor workflow

Primary technical references live in:

- `README.md`
- `docs/contracts/`
- `docs/testing.md`
- `docs/branching.md`

## Commit guidance

- Keep commits atomic.
- Use commit messages that describe the actual engineering change.
- Do not hide risky behavior changes inside “cleanup” commits.

## PR checklist

- The problem is clearly stated.
- The change is scoped.
- Validation commands and outcomes are listed.
- Known risks or follow-ups are listed.
- Docs are updated if behavior or contracts changed.

## What to avoid

- massive formatting-only churn
- speculative abstractions
- changing subsystem ownership for convenience
- weakening capability checks without a documented reason
- merging code that only “seems to boot” without stating what was actually verified

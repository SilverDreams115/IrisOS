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

Three layers gate independently and all three must be green:

```bash
make                    # zero-warning build; a diagnostic is a broken build
make check-purity       # the seL4 purity allowlist — it may only shrink
make test-unit          # host unit suites
```

Plus the runtime lane, which is the gate that matters for capability behaviour
— it runs in ring 3 as a real service and observes the kernel only through
syscalls:

```bash
ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests
```

State the numbers you got in the PR (`SUITE PASS n/n`, the host assertion
count, `[purity] RESULT: OK`), the way the commit log does.

Also useful: `make check` (build/layout validation), `make smoke` (two clean
compile passes, default and selftest-enabled, catching config-specific
breakage), and `make run` for an interactive boot when the change can affect
bring-up.

If you cannot run one of the expected validations, state that explicitly in the PR.

### The purity gate is not advisory

`scripts/purity_allowlist.txt` freezes, per file, how many times productive
kernel code may name `kslab_alloc`.  **The list may only shrink.**  Raising a
count — or moving a use from one file to another — requires amending the
[purity charter](docs/architecture/iris-sel4-purity-charter.md) and the
[ledger](docs/architecture/sel4-convergence-ledger.md) in the same commit, with
a written technical justification.

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

Three documents are **normative** and outrank the rest when they disagree.  A
change that alters what they claim must update them in the same commit:

- [`docs/architecture/iris-sel4-purity-charter.md`](docs/architecture/iris-sel4-purity-charter.md) — what IRIS may never do
- [`docs/architecture/sel4-convergence-roadmap.md`](docs/architecture/sel4-convergence-roadmap.md) — what order the remaining work happens in
- [`docs/architecture/sel4-convergence-ledger.md`](docs/architecture/sel4-convergence-ledger.md) — every non-seL4 mechanism still alive, and when it retires

Other primary technical references:

- `README.md`
- `docs/contracts/`
- `docs/testing.md`
- `docs/branching.md`

Most documents under `docs/architecture/` are **dated phase records**.  Do not
rewrite them when a later stage supersedes them: add a banner at the top saying
what replaced the claim, and leave the record intact.  The reasoning in an
obsolete record is usually why the current design looks the way it does.

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

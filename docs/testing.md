# IRIS testing strategy

This document defines the minimum testing baseline that IRIS must keep green on every change.

## Current test layers

IRIS currently has four practical validation layers, and the numbers below are
what a green tree looks like today (Stage 6 closed):

| Layer | Command | Green means |
|---|---|---|
| Host unit tests | `make test-unit` | 18685 assertions, 0 failed |
| Purity gate | `make check-purity` | allowlist respected (it only ever shrinks) |
| Runtime suite | `make smoke-runtime` | `SUITE PASS 276/276` |
| Runtime + kernel selftests | `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests` | `SUITE PASS 276/276` plus the P3/P41 markers |

The runtime suite is the gate that matters for capability behaviour: it runs in
ring 3 as a real service and observes the kernel only through syscalls.

The three original layers:

1. Static build validation
   - `make clean`
   - `make`
   - `make check`
   - Confirms the UEFI loader, kernel ELF, embedded service ELFs, linker outputs, and ELF layout still build correctly.

2. Reproducible local smoke validation
   - `make smoke`
   - Runs two clean compile passes:
     - default build
     - `ENABLE_RUNTIME_SELFTESTS=1` build
   - This catches regressions where code only compiles in one configuration.

3. Runtime/manual validation
   - `make run`
   - `make run-headless`
   - `make smoke-runtime`
   - `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`
   - Optional deeper local path:
     - `make clean`
     - `make ENABLE_RUNTIME_SELFTESTS=1`
      - `make run`
   - This is the layer that exercises boot, userland bring-up, IRQ delivery, bootstrap handle flow, and service health end-to-end.
   - Healthy-path serial signature now includes:
     - `[IRIS][BOOT] handoff: kernel -> svcmgr/init`
     - `[SVCMGR] ready`
     - `[USER][INIT][BOOT] healthy path OK`

## CI runtime path

GitHub Actions now uses two runtime boot lanes:

- default lane: `make smoke-runtime`
- selftest lane: `make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests`

Both lanes:

- boot the built image in headless QEMU
- capture the serial log to a build artifact
- accept QEMU timeout exit `124` because IRIS intentionally keeps running
- fail if the healthy boot signature is missing

The selftest lane additionally asserts:

- `[IRIS][P3] handle/lifecycle selftests OK`
- `[USER][INIT][DIAG] reply`
- `[SVCMGR][DIAG] kbd status OK`

The interactive `make run` path remains local and developer-oriented.

## Minimum expectations for contributors

For changes that touch build files, boot, kernel, services, handle rights, IRQ routing, initrd contents, or protocol headers:

- run `make smoke`
- run `make run` locally if the change can affect boot/runtime behavior
- prefer `make smoke-runtime` when you need a reproducible headless runtime check

For changes that specifically touch lifecycle, diagnostics, IPC, handle transfer, or service bootstrap:

- prefer the selftest-enabled path:
  - `make clean`
  - `make ENABLE_RUNTIME_SELFTESTS=1`
  - `make run`

## Build-configuration toggling guarantee

Toggling `ENABLE_RUNTIME_SELFTESTS` between invocations is safe and must
keep working on the FIRST `make` after the flip. Stale-artifact cleanup runs
at Makefile **parse time** (see the `BUILD_CONFIG_ON_DISK` block at the top
of the Makefile): a recipe-time cleanup used to delete objects make had
already stat-cached, making the first link after a flip fail on missing
`*_bin.o` until a second invocation. That race is fixed; verified in
Phase V1 with four consecutive first-invocation toggles
(0→1, 1→0, 0→1, 1→0 — all RC=0, zero errors). If a first-invocation toggle
ever fails again, treat it as a regression of this guarantee.

## Current gaps worth closing next

The baseline above is the minimum that should stay green. The next additions
should remain small and directly auditable:

1. Add host-side coverage for protocol packing helpers and authority-reduction helpers that do not require QEMU to validate.
2. Decide whether service-side `IRIS_ENABLE_RUNTIME_SELFTESTS` code should be compiled into the selftest lane as well, and document that policy explicitly.
3. If more boot phases become critical, extend the headless assertion set with one marker per phase boundary rather than relying on free-form log inspection.


## What the convergence stages added to the suite

Each stage's invariants are pinned by named runtime tests, so a regression
names itself rather than showing up as a boot hang:

| Test | Pins |
|---|---|
| T095, T096 | Stage 4's structural zeros: no handle is live, delivered, or produced by a TOCTOU fallback |
| T292–T295 | CSpace-native introspection; a CPtr addresses exactly one capability |
| T296 | Stage 5: one capability, one authority — each boot control capability authorises its own syscall and nothing else |
| T297 | Stage 5: a retyped TCB executes; an unconfigured one cannot be started, written or exited; foreign CSpace/VSpace refused |
| T298 | Stage 6: an Untyped pays for its frames' headers, and frames stay page-dense |
| T299 | Stage 6: page tables are charged to a named budget, which cannot be RESET while they live |
| T300 | Stage 6: user memory comes out of a named budget, and the region is reclaimable once the VMO is gone |
| T301 | Stage 6: a REFUSED spawn leaves its budget untouched — no stranded children, still RESET-able, swept across the boundary in sub-page steps |
| T302 | Stage 6-pure: a page table is a capability — retyped by the holder, installed one level per invocation, refused at a kernel address, and the walk it builds really maps |
| T303 | Stage 7: a running thread outlives every capability to it — the execution reference a retyped TCB never took |
| T304 | Stage 7: the live-process ceiling is gone — more than 64 processes out of one budget, a clean error when that budget ends, and a RESET afterwards that proves nothing leaked |
| PT-1..PT-11 (host) | Stage 6-pure: the paging walk driven exhaustively — level order, spent-vs-complete, kernel-address refusal, dead VSpace, teardown returning every level, the bootstrap exception being one-way, a reused level entering the walk empty, teardown detaching exactly the holder's levels, and a failed composition giving its bind claim back |

Host unit tests cover what a successful boot cannot show: `RBI-1..RBI-10` (the
BootInfo builder's bounds), `UT-TOP-1..5` (the two-ended Untyped carve),
`BC-11..BC-13` (a CSpace that names itself, with a negative control).

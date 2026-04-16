# Initial backlog

## Purpose

This is the current lead-level backlog for turning IRIS into a more verifiable and maintainable engineering project.

## P0

- Headless QEMU smoke in CI with bounded timeout and serial-log assertions.
- Define a machine-checkable healthy-boot signature around `svcmgr`, `vfs`, `kbd`, and `init`.
- Add focused tests for rights reduction, duplicate/transfer edge cases, and stale handle rejection.
- Reconcile the current `kbd` `KIoPort` rights contract with the actual hardware access the service needs.

## P1

- Add host-side tests for protocol encoder/decoder helpers in `svcmgr_proto.h`, `vfs_proto.h`, and `kbd_proto.h`.
- Document handle lifecycle and cleanup invariants closer to code paths that enforce them.
- Add runtime assertions or diagnostics around service restart exhaustion and bootstrap failure causes.
- Define a repeatable promotion checklist for `silver` -> `staging` -> `main`.

## P2

- Add a serial-log capture script for local smoke that preserves artifacts under `build/`.
- Improve service-level boot logs so each bootstrap handle delivery is easier to correlate with service state.
- Add docs for release/promotion notes and expected validation evidence.
- Review whether selected warnings should be promoted to stricter compiler flags over time.

## Not in scope until foundations are stronger

- feature work that does not improve verification, contracts, or stability
- large subsystem rewrites without a failing validation or hard architectural reason

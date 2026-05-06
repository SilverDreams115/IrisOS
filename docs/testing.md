# IRIS testing strategy

This document defines the minimum testing baseline that IRIS must keep green on every change.

## Current test layers

IRIS currently has three practical validation layers:

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

## Current gaps worth closing next

The baseline above is the minimum that should stay green. The next additions
should remain small and directly auditable:

1. Add host-side coverage for protocol packing helpers and authority-reduction helpers that do not require QEMU to validate.
2. Decide whether service-side `IRIS_ENABLE_RUNTIME_SELFTESTS` code should be compiled into the selftest lane as well, and document that policy explicitly.
3. If more boot phases become critical, extend the headless assertion set with one marker per phase boundary rather than relying on free-form log inspection.

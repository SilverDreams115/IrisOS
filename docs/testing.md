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

GitHub Actions now uses `make smoke-runtime`, which:

- boots the built image in headless QEMU
- captures the serial log to `build/qemu-headless.log`
- accepts QEMU timeout exit `124` because IRIS intentionally keeps running
- fails if the healthy boot signature is missing

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

## Immediate testing roadmap

The next testing increments should stay small and verifiable:

1. Promote selected selftest-enabled runtime checks into a second automated CI boot lane.
2. Add stronger serial assertions for failure-stage localization, not only success markers.
3. Add focused host-side tests for protocol encoding/decoding helpers and rights reduction logic where that can be done without emulation.

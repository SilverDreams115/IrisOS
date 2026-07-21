# kernel/drivers — Mixed state

**The only real driver:** `serial/serial.c` — a 16550 UART driver for serial
debug. Correctly in the kernel because the debug console predates any server.

**All other subdirectories are empty (only `.gitkeep`):**

- `acpi/` — Not implemented. ACPI parsing would require a full AML table.
  In a proper microkernel, ACPI is handled by a user-space server. For now
  boot uses the bootloader (UEFI) information directly.
- `audio/` — Not implemented. Out of scope for Phase 0-2.
- `dma/` — Not implemented. There are no active DMA devices in the kernel today.
- `framebuffer/` — The framebuffer is managed by the `services/fb/` service.
  There must be no framebuffer driver in the kernel except the base-address
  initialization (passed by the bootloader).
- `gpu/` (amd/, core/, intel/, nvidia/) — Not implemented. The GPU is user-space,
  always. A premature placeholder that gives a false impression of GPU support.
- `input/` — Not implemented. The PS/2 keyboard lives in `services/kbd/`.
- `keyboard/` — Duplicate of `input/`. Both empty.
- `pci/` — Not implemented. PCI enumeration is needed before any PCIe driver.
  A real candidate for Phase 2, but only if there are PCIe devices the kernel
  must handle directly (rare in a microkernel).

**Phase 0 decision:** Do not add any new driver. Only `serial.c` stays.

**Risk:** The presence of `gpu/nvidia/` etc. creates a false expectation of
support. Phase 1 proposal: remove driver subdirectories that are not planned
for the next 3 roadmap phases.

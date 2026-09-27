# kernel/drivers — Mixed state

**The only real driver:** `serial/serial.c` — a 16550 UART driver for serial
debug. Correctly in the kernel because the debug console predates any server.

**All other subdirectories are empty (only `.gitkeep`):**

- `acpi/` — Empty. The kernel DOES read ACPI, in `kernel/core/acpi/`: enough
  of the fixed tables to find the LAPIC, the IOAPICs and the DMAR remapping
  units, which are things it cannot ask a server about because they are what
  it needs to start one. It interprets no AML, and anything that would need
  an interpreter belongs in a server.
- `audio/` — Not implemented, and out of scope: sound is a user-space server.
- `dma/` — Not implemented. There are no active DMA devices in the kernel today.
- `framebuffer/` — The framebuffer is managed by the `services/fb/` service.
  There must be no framebuffer driver in the kernel except the base-address
  initialization (passed by the bootloader).
- `gpu/` (amd/, core/, intel/, nvidia/) — Not implemented. The GPU is user-space,
  always. A premature placeholder that gives a false impression of GPU support.
- `input/` — Not implemented. The PS/2 keyboard lives in `services/kbd/`.
- `keyboard/` — Duplicate of `input/`. Both empty.
- `pci/` — Empty, and it stays empty. PCI enumeration is the `pci` SERVICE in
  ring 3, which holds the configuration ports and the device Untypeds over the
  PCI windows and is the only task that can reach either. The kernel
  enumerates no bus.

**Decision:** Do not add any new driver. Only `serial.c` stays.

**Risk:** The presence of `gpu/nvidia/` etc. creates a false expectation of
support. The empty subdirectories should be removed, not filled.

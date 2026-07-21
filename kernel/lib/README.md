# kernel/lib — PLACEHOLDER (not implemented)

**State:** Empty. Contains only `.gitkeep` subdirectories.

**Intended purpose:** Utilities shared by the kernel that do not fit in any
specific subsystem.

**Subdirectories and their reality:**

- `bitmap/` — Not implemented. The scheduler uses a hand-rolled bitmap in
  CpuRunQueue.mask[4]. If it is generalized, the implementation would go here.
- `elf/` — Not implemented. The kernel does not parse ELF today; services are
  loaded as flat binaries from the initrd. If an ELF loader is added to the
  kernel, it goes here.
- `printf/` — Not implemented as a separate lib. The kernel uses serial.c +
  klog.c directly. If a freestanding printf is needed, it goes here.
- `string/` — Not implemented. The kernel uses manual memory-copy operations.
  If memcpy/memset are added as functions, they go here.

**Phase 0 decision:** Do not implement. The current uses do not need this
abstraction.

**Future decision:** Implement only if there is clear duplication between two
subsystems. Do not create it preemptively.

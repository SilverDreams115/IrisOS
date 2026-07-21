# kernel/fs — PLACEHOLDER (not implemented)

**State:** Empty. Contains only `.gitkeep` subdirectories.

**Architectural risk:** This directory contradicts the IRIS microkernel model.

In IRIS the filesystem is the responsibility of the VFS server
(`services/vfs/`), not the kernel. The kernel does not, and must not,
implement any filesystem: its only "storage" interface is the initrd, handled
by `kernel/core/initrd/`.

**Subdirectories and their reality:**

- `irisfs/` — Placeholder for a hypothetical native IRIS filesystem. No code
  exists. The team must not start irisfs until the VFS server has a stable
  interface and a concrete use case (persistence vs. RAM).
- `ramfs/` — Placeholder for an in-kernel ramfs. The initrd fills this role
  today. A user-space ramfs (in the VFS server) would be the correct place.
- `vfs/` — The real VFS is in `services/vfs/`, NOT here. This directory is empty.

**Phase 0 decision:** Implement nothing here.

**Future decision:** Consider removing `kernel/fs/` entirely. The VFS lives in
`services/vfs/`. If an in-kernel filesystem is ever needed (e.g. for early
debug), document it as a justified exception before adding code.

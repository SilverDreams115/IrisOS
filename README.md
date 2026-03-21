# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, framebuffer graphics, and filesystem abstractions.

---

## Current Status

**Stage 10 - VFS + ramfs**

The kernel now includes a virtual filesystem layer backed by an in-memory ramfs. Files can be created, written, read, and stat'd through a clean VFS API. The VFS, IPC, scheduler, and framebuffer all run together at boot.

### Completed stages

| Stage | Component | Description |
|-------|-----------|-------------|
| 0-2 | UEFI Bootstrap | Loader parses ELF kernel, passes `iris_boot_info`, jumps to kernel |
| 3 | ExitBootServices | Firmware OFF, kernel owns machine, full memory map passed |
| 4 | PMM | Bitmap allocator, 503 MB tracked at 4K granularity |
| 5 | Paging | 4-level x86_64 paging, 2MB huge pages, kernel higher half mapped |
| 6 | GDT + IDT | 5-entry GDT, 256-entry IDT, exception handlers with serial output |
| 7 | Scheduler | Round-robin cooperative+preemptive scheduler, PIT at 100Hz |
| 8 | IPC | Circular buffer channels, blocking recv, producer/consumer validated |
| 9 | Framebuffer | Direct pixel writes to UEFI GOP framebuffer, `fb_fill`, `fb_draw_rect` |
| 10 | VFS + ramfs | Virtual filesystem layer, in-memory ramfs, open/read/write/close/stat |

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `0xFFFFFFFF80200000`, framebuffer at `0x80000000`
- **Scheduler:** Cooperative + preemptive (PIT IRQ0 at 100Hz), round-robin
- **IPC:** In-kernel circular buffer channels with blocking `ipc_recv` and non-blocking `ipc_send`
- **Framebuffer:** UEFI GOP base mapped at boot, `pixels_per_scanline` stride, ARGB pixel format
- **VFS:** Thin abstraction layer over ramfs, POSIX-inspired API (open/read/write/close/stat)
- **ramfs:** Static inode table, 32 files max, 4KB per file, zero dynamic allocation

---

## Repository Structure
```
boot/uefi/boot.c                         UEFI loader (silent, passes iris_boot_info)
kernel/kernel_main.c                     Kernel entry, subsystem init, stage demos
kernel/include/iris/                     All kernel headers
kernel/mm/pmm/pmm.c                      Bitmap PMM
kernel/arch/x86_64/paging.c             4-level paging + huge pages
kernel/arch/x86_64/gdt.c                GDT setup
kernel/arch/x86_64/idt.c                IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                PIC remap + PIT
kernel/arch/x86_64/isr_stubs.S          32 ISR stubs + IRQ0
kernel/arch/x86_64/context_switch.S     context_switch(old, new)
kernel/core/scheduler/scheduler.c       Round-robin scheduler
kernel/core/ipc/ipc.c                   IPC channels
kernel/drivers/fb/fb.c                  Framebuffer driver
kernel/fs/ramfs/ramfs.c                 In-memory filesystem (inodes + data buffers)
kernel/fs/ramfs/vfs.c                   VFS layer (fd table, open/read/write/close/stat)
```

---

## Build & Run

**Requirements:** gcc, ld, gnu-efi, OVMF, QEMU with GTK display
```bash
make clean && make && make run
```

QEMU launches with a GTK window showing the framebuffer. Serial output goes to stdout.

---

## Stage 10 Demo

On boot the kernel:
1. Initializes PMM, paging, GDT, IDT, PIC, PIT
2. Paints 7 color stripes to the framebuffer
3. Initializes VFS + ramfs
4. Creates `iris.txt`, writes "Hello from IrisOS VFS!", reads it back, stats it
5. Creates IPC producer/consumer tasks that run indefinitely under the scheduler

Serial output:
```
[IRIS][VFS] read iris.txt (23 bytes): Hello from IrisOS VFS!
[IRIS][VFS] stat iris.txt size=23
[IRIS][VFS] OK
```

---

## Roadmap
```
Stage 11  Userland init process
Stage 12  Syscall interface
Stage 13  Driver model (PCI enumeration)
Stage 14  GPU / Audio / Input drivers
```

---

## Branch Strategy

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration + testing |
| `main` | Stable releases |
| `collab` | External contributors |

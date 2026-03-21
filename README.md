# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, framebuffer graphics, filesystem abstractions, and userland execution.

---

## Current Status

**Stage 11 - Userland Ring 3**

The kernel now launches a real user-space process running in CPU privilege level 3 (ring 3). The transition uses a proper TSS, kernel stack per task, and `iretq` to drop privileges. The user init process runs concurrently with the kernel scheduler and IPC tasks.

### Completed stages

| Stage | Component | Description |
|-------|-----------|-------------|
| 0-2 | UEFI Bootstrap | Loader parses ELF kernel, passes `iris_boot_info`, jumps to kernel |
| 3 | ExitBootServices | Firmware OFF, kernel owns machine, full memory map passed |
| 4 | PMM | Bitmap allocator, 503 MB tracked at 4K granularity, `pmm_alloc_pages(n)` |
| 5 | Paging | 4-level x86_64 paging, 2MB huge pages, kernel higher half mapped |
| 6 | GDT + IDT | 5-entry GDT, 256-entry IDT, exception handlers with serial output |
| 7 | Scheduler | Round-robin cooperative+preemptive scheduler, PIT at 100Hz |
| 8 | IPC | Circular buffer channels, blocking recv, producer/consumer validated |
| 9 | Framebuffer | Direct pixel writes to UEFI GOP framebuffer, `fb_fill`, `fb_draw_rect` |
| 10 | VFS + ramfs | Virtual filesystem layer, in-memory ramfs, open/read/write/close/stat/seek |
| 11 | Userland | TSS, kernel stack per task, `iretq` trampoline, ring 3 user_init process |

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `0xFFFFFFFF80200000`, framebuffer at `0x80000000`
- **Scheduler:** Cooperative + preemptive (PIT IRQ0 at 100Hz), round-robin, ring 0 + ring 3 tasks
- **IPC:** In-kernel circular buffer channels with blocking `ipc_recv` and non-blocking `ipc_send`
- **Framebuffer:** UEFI GOP base mapped at boot, `pixels_per_scanline` stride, ARGB pixel format
- **VFS:** Thin abstraction layer over ramfs, POSIX-inspired API (open/read/write/close/stat/seek)
- **ramfs:** Static inode table, 32 files max, 4KB per file, zero dynamic allocation
- **Userland:** TSS with per-task `rsp0`, `iretq` trampoline zeroing all GPRs, CS=0x1B SS=0x23

---

## Repository Structure
```
boot/uefi/boot.c                          UEFI loader (silent, passes iris_boot_info)
kernel/kernel_main.c                      Kernel entry, subsystem init, stage demos
kernel/include/iris/                      All kernel headers
kernel/mm/pmm/pmm.c                       Bitmap PMM + pmm_alloc_pages(n)
kernel/arch/x86_64/paging.c              4-level paging + huge pages
kernel/arch/x86_64/gdt.c                 GDT + TSS setup (7 entries)
kernel/arch/x86_64/idt.c                 IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                 PIC remap + PIT
kernel/arch/x86_64/gdt_idt_flush.S       gdt_flush, idt_flush, tss_flush
kernel/arch/x86_64/isr_stubs.S           32 ISR stubs + IRQ0
kernel/arch/x86_64/context_switch.S      context_switch(old, new)
kernel/arch/x86_64/user_trampoline.S     iretq trampoline to ring 3
kernel/arch/x86_64/user_init.S           First user-space function (ring 3)
kernel/core/scheduler/scheduler.c        Round-robin scheduler, task_create_user()
kernel/core/ipc/ipc.c                    IPC channels
kernel/drivers/fb/fb.c                   Framebuffer driver
kernel/fs/ramfs/ramfs.c                  In-memory filesystem
kernel/fs/ramfs/vfs.c                    VFS layer
```

---

## Build & Run

**Requirements:** gcc, ld, gnu-efi, OVMF, QEMU with GTK display
```bash
make clean && make && make run
```

QEMU launches with a GTK window showing the framebuffer. Serial output goes to stdout.

---

## Stage 11 Boot Sequence
```
====================================
       IRIS KERNEL - STAGE 11
====================================
[IRIS][KERNEL] firmware services: OFF
[IRIS][PMM] initializing...        — 505 MB free
[IRIS][PAGING] virtual memory active
[IRIS][GDT] OK                     — TSS loaded at selector 0x28
[IRIS][IPC] channels: 0 and 1
[IRIS][FB] framebuffer painted     — 7 color stripes on screen
[IRIS][VFS] read iris.txt (23 bytes): Hello from IrisOS VFS!
[IRIS][USER] init task created, id=3  — ring 3 process running
[IRIS][SCHED] IPC running
[PRODUCER] sent #1 ...             — IPC running alongside user task
```

---

## Roadmap
```
Stage 12  Syscall interface (syscall/sysret MSR)
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

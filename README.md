# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, framebuffer graphics, filesystem abstractions, userland execution, syscall interface, and hardware enumeration.

---

## Current Status

**Stage 13 - PCI Enumeration**

The kernel now enumerates all PCI devices on boot using config space I/O ports. On QEMU it finds 4 devices: Intel host bridge, QEMU VGA display, Intel e1000 network card, and ICH9 LPC bridge. Results are printed to serial with clean hex formatting.

### Completed stages

| Stage | Component | Description |
|-------|-----------|-------------|
| 0-2 | UEFI Bootstrap | Loader parses ELF kernel, passes `iris_boot_info`, jumps to kernel |
| 3 | ExitBootServices | Firmware OFF, kernel owns machine, full memory map passed |
| 4 | PMM | Bitmap allocator, 503 MB tracked at 4K granularity, `pmm_alloc_pages(n)` |
| 5 | Paging | 4-level x86_64 paging, 2MB huge pages, kernel + user pages mapped |
| 6 | GDT + IDT | 7-entry GDT with TSS, 256-entry IDT, exception handlers |
| 7 | Scheduler | Round-robin cooperative+preemptive scheduler, PIT at 100Hz |
| 8 | IPC | Circular buffer channels, blocking recv, producer/consumer validated |
| 9 | Framebuffer | Direct pixel writes to UEFI GOP framebuffer, `fb_fill`, `fb_draw_rect` |
| 10 | VFS + ramfs | Virtual filesystem, in-memory ramfs, open/read/write/close/stat/seek/mkdir |
| 11 | Userland | TSS, kernel stack per task, `iretq` trampoline, ring 3 user_init process |
| 12 | Syscall | MSR LSTAR/STAR/SFMASK, syscall entry ASM, SYS_WRITE/SYS_EXIT/SYS_GETPID |
| 13 | PCI | Bus enumeration via config space, device table, class detection |

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `0xFFFFFFFF80200000`, framebuffer at `0x80000000`, user pages with U/S bit
- **Scheduler:** Cooperative + preemptive (PIT IRQ0 at 100Hz), round-robin, ring 0 + ring 3 tasks
- **IPC:** In-kernel circular buffer channels
- **Framebuffer:** UEFI GOP, `pixels_per_scanline` stride, ARGB
- **VFS:** POSIX-inspired API backed by ramfs, supports files and directories
- **Syscall:** MSR-based (`syscall`/`sysretq`), user pointer validation, C dispatcher
- **PCI:** Config space enumeration (ports 0xCF8/0xCFC), multi-function support, class detection

---

## Repository Structure
```
boot/uefi/boot.c                          UEFI loader
kernel/kernel_main.c                      Kernel entry, subsystem init
kernel/include/iris/                      All kernel headers
kernel/mm/pmm/pmm.c                       Bitmap PMM
kernel/arch/x86_64/paging.c              4-level paging + huge pages + USER bit
kernel/arch/x86_64/gdt.c                 GDT + TSS
kernel/arch/x86_64/idt.c                 IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                 PIC remap + PIT
kernel/arch/x86_64/syscall_entry.S       MSR syscall handler
kernel/arch/x86_64/user_trampoline.S     iretq trampoline to ring 3
kernel/arch/x86_64/user_init.S           First user-space process
kernel/arch/x86_64/context_switch.S      context_switch(old, new)
kernel/core/scheduler/scheduler.c        Round-robin scheduler
kernel/core/ipc/ipc.c                    IPC channels
kernel/core/syscall/syscall.c            Syscall dispatcher
kernel/drivers/fb/fb.c                   Framebuffer driver
kernel/drivers/serial/serial.c           Serial output (hex8/hex16/hex64/dec)
kernel/drivers/pci/pci.c                 PCI bus enumeration + device table
kernel/fs/ramfs/ramfs.c                  In-memory filesystem
kernel/fs/ramfs/vfs.c                    VFS layer
```

---

## Build & Run
```bash
make clean && make && make run
```

---

## Stage 13 Boot Output
```
====================================
       IRIS KERNEL - STAGE 13
====================================
[IRIS][PCI] scanning buses...
[PCI] 0:0.0   vendor=0x8086 device=0x29C0 [Bridge]
[PCI] 0:1.0   vendor=0x1234 device=0x1111 [Display]
[PCI] 0:2.0   vendor=0x8086 device=0x10D3 [Network]
[PCI] 0:31.0  vendor=0x8086 device=0x2918 [Bridge]
[IRIS][PCI] found 4 device(s)
[USER] Hello from ring 3!
```

---

## PCI Device Table (QEMU)

| Bus:Dev.Func | Vendor | Device | Class |
|-------------|--------|--------|-------|
| 0:0.0 | 0x8086 (Intel) | 0x29C0 | Host Bridge |
| 0:1.0 | 0x1234 (QEMU) | 0x1111 | VGA Display |
| 0:2.0 | 0x8086 (Intel) | 0x10D3 | e1000 Network |
| 0:31.0 | 0x8086 (Intel) | 0x2918 | ICH9 LPC Bridge |

---

## Syscall Interface

| Number | Name | Args | Description |
|--------|------|------|-------------|
| 0 | SYS_WRITE | rdi=str_ptr | Print string to serial (validated ptr) |
| 1 | SYS_EXIT | rdi=code | Terminate process |
| 2 | SYS_GETPID | — | Return current task id |

---

## Roadmap
```
Stage 14  Network / Storage / Input drivers
```

---

## Branch Strategy

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration + testing |
| `main` | Stable releases |
| `collab` | External contributors |

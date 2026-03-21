# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, framebuffer graphics, filesystem abstractions, userland execution, and syscall interface.

---

## Current Status

**Stage 12 - Syscall Interface**

The kernel now has a fully working syscall interface using the `syscall`/`sysretq` MSR mechanism. User processes running in ring 3 can call into the kernel via `syscall`, get dispatched to C handlers, and return to user space cleanly. Validated with `SYS_WRITE` printing from ring 3 to serial output.

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

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `0xFFFFFFFF80200000`, framebuffer at `0x80000000`, user pages with U/S bit set
- **Scheduler:** Cooperative + preemptive (PIT IRQ0 at 100Hz), round-robin, ring 0 + ring 3 tasks
- **IPC:** In-kernel circular buffer channels with blocking `ipc_recv` and non-blocking `ipc_send`
- **Framebuffer:** UEFI GOP base mapped at boot, `pixels_per_scanline` stride, ARGB pixel format
- **VFS:** POSIX-inspired API (open/read/write/close/stat/seek/mkdir), backed by ramfs
- **ramfs:** Static inode table, 32 files/dirs max, 4KB per file, zero dynamic allocation
- **Userland:** TSS with per-task `rsp0`, `iretq` trampoline zeroing all GPRs, IOPL=3
- **Syscall:** MSR-based (`syscall`/`sysretq`), LSTAR handler in ASM, C dispatcher, user pointer validation

---

## Repository Structure
```
boot/uefi/boot.c                          UEFI loader
kernel/kernel_main.c                      Kernel entry, subsystem init
kernel/include/iris/                      All kernel headers
kernel/mm/pmm/pmm.c                       Bitmap PMM + pmm_alloc_pages(n)
kernel/arch/x86_64/paging.c              4-level paging + huge pages + USER bit
kernel/arch/x86_64/gdt.c                 GDT + TSS (7 entries)
kernel/arch/x86_64/idt.c                 IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                 PIC remap + PIT
kernel/arch/x86_64/gdt_idt_flush.S       gdt_flush, idt_flush, tss_flush
kernel/arch/x86_64/isr_stubs.S           32 ISR stubs + IRQ0
kernel/arch/x86_64/context_switch.S      context_switch(old, new)
kernel/arch/x86_64/user_trampoline.S     iretq trampoline to ring 3
kernel/arch/x86_64/user_init.S           First user-space process (ring 3)
kernel/arch/x86_64/syscall_entry.S       MSR syscall handler, stack switch, sysretq
kernel/core/scheduler/scheduler.c        Round-robin scheduler, task_create_user()
kernel/core/ipc/ipc.c                    IPC channels
kernel/core/syscall/syscall.c            Syscall dispatcher (write/exit/getpid)
kernel/drivers/fb/fb.c                   Framebuffer driver
kernel/drivers/serial/serial.c           Serial output driver (COM1)
kernel/fs/ramfs/ramfs.c                  In-memory filesystem + mkdir
kernel/fs/ramfs/vfs.c                    VFS layer
```

---

## Build & Run

**Requirements:** gcc, ld, gnu-efi, OVMF, QEMU with GTK display
```bash
make clean && make && make run
```

---

## Stage 12 Boot Output
```
====================================
       IRIS KERNEL - STAGE 12
====================================
[IRIS][PMM] free RAM: 505 MB
[IRIS][PAGING] virtual memory active
[IRIS][GDT] OK                     — TSS at selector 0x28
[IRIS][VFS] /dev created
[IRIS][VFS] read iris.txt (23 bytes): Hello from IrisOS VFS!
[IRIS][SYSCALL] MSRs configured    — LSTAR, STAR, SFMASK set
[IRIS][USER] init task created, id=3
[PRODUCER] sent #1 ...
[USER] Hello from ring 3!          — SYS_WRITE from ring 3 via syscall
```

---

## Syscall Interface

| Number | Name | Args | Description |
|--------|------|------|-------------|
| 0 | SYS_WRITE | rdi=str_ptr | Print string to serial (validated ptr) |
| 1 | SYS_EXIT | rdi=code | Terminate process |
| 2 | SYS_GETPID | — | Return current task id |

Calling convention: `rax=syscall_num, rdi=arg0, rsi=arg1, rdx=arg2`

---

## Roadmap
```
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
